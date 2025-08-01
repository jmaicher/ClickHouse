#include <Columns/IColumn.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <Functions/IFunction.h>
#include <Interpreters/ActionsDAG.h>
#include <Interpreters/ArrayJoinAction.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/TableJoin.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTWindowDefinition.h>
#include <Processors/QueryPlan/AggregatingStep.h>
#include <Processors/QueryPlan/ArrayJoinStep.h>
#include <Processors/QueryPlan/CreatingSetsStep.h>
#include <Processors/QueryPlan/CubeStep.h>
#include <Processors/QueryPlan/DistinctStep.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/QueryPlan/FilterStep.h>
#include <Processors/QueryPlan/ITransformingStep.h>
#include <Processors/QueryPlan/Optimizations/Optimizations.h>
#include <Processors/QueryPlan/Optimizations/actionsDAGUtils.h>
#include <Processors/QueryPlan/ReadFromMergeTree.h>
#include <Processors/QueryPlan/ReadFromRemote.h>
#include <Processors/QueryPlan/SortingStep.h>
#include <Processors/QueryPlan/TotalsHavingStep.h>
#include <Processors/QueryPlan/UnionStep.h>
#include <Processors/QueryPlan/WindowStep.h>
#include <Storages/ReadInOrderOptimizer.h>
#include <Storages/KeyDescription.h>
#include <Storages/StorageMerge.h>
#include <Common/typeid_cast.h>
#include <Core/Settings.h>

#include <stack>

namespace DB
{
namespace Setting
{
    extern const SettingsBool allow_experimental_analyzer;
    extern const SettingsBool query_plan_read_in_order;
    extern const SettingsBool optimize_read_in_order;
    extern const SettingsBool optimize_read_in_window_order;
}
}

namespace DB::QueryPlanOptimizations
{

namespace
{

ISourceStep * checkSupportedReadingStep(IQueryPlanStep * step, bool allow_existing_order)
{
    if (auto * reading = typeid_cast<ReadFromMergeTree *>(step))
    {
        /// Already read-in-order, skip.
        if (!allow_existing_order && reading->getQueryInfo().input_order_info)
            return nullptr;

        const auto & sorting_key = reading->getStorageMetadata()->getSortingKey();
        if (sorting_key.column_names.empty())
            return nullptr;

        return reading;
    }

    if (auto * merge = typeid_cast<ReadFromMerge *>(step))
    {
        if (const auto & order = merge->getInputOrder();
            !allow_existing_order && order && order->used_prefix_of_sorting_key_size)
            return nullptr;

        const auto & tables = merge->getSelectedTables();
        if (tables.empty())
            return nullptr;

        for (const auto & table : tables)
        {
            auto storage = std::get<StoragePtr>(table);
            const auto & sorting_key = storage->getInMemoryMetadataPtr()->getSortingKey();
            if (sorting_key.column_names.empty())
                return nullptr;
        }

        return merge;
    }

    return nullptr;
}

using StepStack = std::vector<IQueryPlanStep*>;

QueryPlan::Node * findReadingStep(QueryPlan::Node & node, bool allow_existing_order)
{
    IQueryPlanStep * step = node.step.get();
    if (auto * /*reading*/ _ = checkSupportedReadingStep(step, allow_existing_order))
        return &node;

    if (node.children.size() != 1)
        return nullptr;

    if (typeid_cast<ExpressionStep *>(step) || typeid_cast<FilterStep *>(step) || typeid_cast<ArrayJoinStep *>(step))
        return findReadingStep(*node.children.front(), allow_existing_order);

    if (auto * distinct = typeid_cast<DistinctStep *>(step); distinct && distinct->isPreliminary())
        return findReadingStep(*node.children.front(), allow_existing_order);

    return nullptr;
}

/// FixedColumns are columns which values become constants after filtering.
/// In a query "SELECT x, y, z FROM table WHERE x = 1 AND y = 'a' ORDER BY x, y, z"
/// Fixed columns are 'x' and 'y'.
using FixedColumns = std::unordered_set<const ActionsDAG::Node *>;

/// Right now we find only simple cases like 'and(..., and(..., and(column = value, ...), ...'
/// Injective functions are supported here. For a condition 'injectiveFunction(x) = 5' column 'x' is fixed.
void appendFixedColumnsFromFilterExpression(const ActionsDAG::Node & filter_expression, FixedColumns & fixed_columns)
{
    std::stack<const ActionsDAG::Node *> stack;
    stack.push(&filter_expression);

    while (!stack.empty())
    {
        const auto * node = stack.top();
        stack.pop();
        if (node->type == ActionsDAG::ActionType::FUNCTION)
        {
            const auto & name = node->function_base->getName();
            if (name == "and")
            {
                for (const auto * arg : node->children)
                    stack.push(arg);
            }
            else if (name == "equals")
            {
                const ActionsDAG::Node * maybe_fixed_column = nullptr;
                size_t num_constant_columns = 0;
                for (const auto & child : node->children)
                {
                    if (child->column)
                        ++num_constant_columns;
                    else
                        maybe_fixed_column = child;
                }

                if (maybe_fixed_column && num_constant_columns + 1 == node->children.size())
                {
                    //std::cerr << "====== Added fixed column " << maybe_fixed_column->result_name << ' ' << static_cast<const void *>(maybe_fixed_column) << std::endl;
                    fixed_columns.insert(maybe_fixed_column);

                    /// Support injective functions chain.
                    const ActionsDAG::Node * maybe_injective = maybe_fixed_column;
                    while (maybe_injective->type == ActionsDAG::ActionType::FUNCTION
                        && maybe_injective->children.size() == 1
                        && maybe_injective->function_base->isInjective({}))
                    {
                        maybe_injective = maybe_injective->children.front();
                        fixed_columns.insert(maybe_injective);
                    }
                }
            }
        }
    }
}

void appendExpression(std::optional<ActionsDAG> & dag, const ActionsDAG & expression)
{
    if (dag)
        dag->mergeInplace(expression.clone());
    else
        dag = expression.clone();
}

/// This function builds a common DAG which is a merge of DAGs from Filter and Expression steps chain.
/// Additionally, build a set of fixed columns.
void buildSortingDAG(QueryPlan::Node & node, std::optional<ActionsDAG> & dag, FixedColumns & fixed_columns, size_t & limit)
{
    IQueryPlanStep * step = node.step.get();
    if (auto * reading = typeid_cast<ReadFromMergeTree *>(step))
    {
        if (const auto prewhere_info = reading->getPrewhereInfo())
        {
            /// Should ignore limit if there is filtering.
            limit = 0;

            //std::cerr << "====== Adding prewhere " << std::endl;
            appendExpression(dag, prewhere_info->prewhere_actions);
            if (const auto * filter_expression = dag->tryFindInOutputs(prewhere_info->prewhere_column_name))
                appendFixedColumnsFromFilterExpression(*filter_expression, fixed_columns);

        }
        return;
    }

    if (node.children.size() != 1)
        return;

    buildSortingDAG(*node.children.front(), dag, fixed_columns, limit);

    if (typeid_cast<DistinctStep *>(step))
    {
        limit = 0;
    }

    if (auto * expression = typeid_cast<ExpressionStep *>(step))
    {
        const auto & actions = expression->getExpression();

        /// Should ignore limit because arrayJoin() can reduce the number of rows in case of empty array.
        if (actions.hasArrayJoin())
            limit = 0;

        appendExpression(dag, actions);
    }

    if (auto * filter = typeid_cast<FilterStep *>(step))
    {
        /// Should ignore limit if there is filtering.
        limit = 0;

        appendExpression(dag, filter->getExpression());
        if (const auto * filter_expression = dag->tryFindInOutputs(filter->getFilterColumnName()))
            appendFixedColumnsFromFilterExpression(*filter_expression, fixed_columns);
    }

    if (auto * array_join = typeid_cast<ArrayJoinStep *>(step))
    {
        /// Should ignore limit because ARRAY JOIN can reduce the number of rows in case of empty array.
        /// But in case of LEFT ARRAY JOIN the result number of rows is always bigger.
        if (!array_join->isLeft())
            limit = 0;

        const auto & array_joined_columns = array_join->getColumns();

        if (dag)
        {
            std::unordered_set<std::string_view> keys_set(array_joined_columns.begin(), array_joined_columns.end());

            /// Remove array joined columns from outputs.
            /// Types are changed after ARRAY JOIN, and we can't use this columns anyway.
            ActionsDAG::NodeRawConstPtrs outputs;
            outputs.reserve(dag->getOutputs().size());

            for (const auto & output : dag->getOutputs())
            {
                if (!keys_set.contains(output->result_name))
                    outputs.push_back(output);
            }

            dag->getOutputs() = std::move(outputs);
        }
    }
}

/// Add more functions to fixed columns.
/// Functions result is fixed if all arguments are fixed or constants.
void enrichFixedColumns(const ActionsDAG & dag, FixedColumns & fixed_columns)
{
    struct Frame
    {
        const ActionsDAG::Node * node;
        size_t next_child = 0;
    };

    std::stack<Frame> stack;
    std::unordered_set<const ActionsDAG::Node *> visited;
    for (const auto & node : dag.getNodes())
    {
        if (visited.contains(&node))
            continue;

        stack.push({&node});
        visited.insert(&node);
        while (!stack.empty())
        {
            auto & frame = stack.top();
            for (; frame.next_child < frame.node->children.size(); ++frame.next_child)
                if (!visited.contains(frame.node->children[frame.next_child]))
                    break;

            if (frame.next_child < frame.node->children.size())
            {
                const auto * child = frame.node->children[frame.next_child];
                visited.insert(child);
                stack.push({child});
                ++frame.next_child;
            }
            else
            {
                /// Ignore constants here, will check them separately
                if (!frame.node->column)
                {
                    if (frame.node->type == ActionsDAG::ActionType::ALIAS)
                    {
                        if (fixed_columns.contains(frame.node->children.at(0)))
                            fixed_columns.insert(frame.node);
                    }
                    else if (frame.node->type == ActionsDAG::ActionType::FUNCTION)
                    {
                        if (frame.node->function_base->isDeterministicInScopeOfQuery())
                        {
                            //std::cerr << "*** enrichFixedColumns check " << frame.node->result_name << std::endl;
                            bool all_args_fixed_or_const = true;
                            for (const auto * child : frame.node->children)
                            {
                                if (!child->column && !fixed_columns.contains(child))
                                {
                                    //std::cerr << "*** enrichFixedColumns fail " << child->result_name <<  ' ' << static_cast<const void *>(child) << std::endl;
                                    all_args_fixed_or_const = false;
                                }
                            }

                            if (all_args_fixed_or_const)
                            {
                                //std::cerr << "*** enrichFixedColumns add " << frame.node->result_name << ' ' << static_cast<const void *>(frame.node) << std::endl;
                                fixed_columns.insert(frame.node);
                            }
                        }
                    }
                }

                stack.pop();
            }
        }
    }
}

const ActionsDAG::Node * addMonotonicChain(ActionsDAG & dag, const ActionsDAG::Node * node, const MatchedTrees::Match * match, const std::string & input_name)
{
    if (!match->monotonicity)
        return &dag.addInput(input_name, node->result_type);

    if (node->type == ActionsDAG::ActionType::ALIAS)
        return &dag.addAlias(*addMonotonicChain(dag, node->children.front(), match, input_name), node->result_name);

    ActionsDAG::NodeRawConstPtrs args;
    args.reserve(node->children.size());
    for (const auto * child : node->children)
    {
        if (child == match->monotonicity->child_node)
            args.push_back(addMonotonicChain(dag, match->monotonicity->child_node, match->monotonicity->child_match, input_name));
        else
            args.push_back(&dag.addColumn({child->column, child->result_type, child->result_name}));
    }

    return &dag.addFunction(node->function_base, std::move(args), {});
}

struct SortingInputOrder
{
    InputOrderInfoPtr input_order{};
    /// This is needed for virtual row optimization.
    /// Convert the PR values to ORDER BY key.
    /// If empty, the optimization cannot be applied.
    std::optional<ActionsDAG> virtual_row_conversion{};
};

/// For the case when the order of keys is important (ORDER BY keys).
SortingInputOrder buildInputOrderFromSortDescription(
    const FixedColumns & fixed_columns,
    const std::optional<ActionsDAG> & dag,
    const SortDescription & description,
    const KeyDescription & sorting_key,
    const Names & pk_column_names,
    size_t limit)
{
    //std::cerr << "------- buildInputOrderInfo " << std::endl;
    SortDescription order_key_prefix_descr;
    order_key_prefix_descr.reserve(description.size());

    MatchedTrees::Matches matches;
    FixedColumns fixed_key_columns;

    const auto & sorting_key_dag = sorting_key.expression->getActionsDAG();

    if (dag)
    {
        matches = matchTrees(sorting_key_dag.getOutputs(), *dag);

        for (const auto & [node, match] : matches)
        {
            //std::cerr << "------- matching " << static_cast<const void *>(node) << " " << node->result_name
            //    << " to " << static_cast<const void *>(match.node) << " " << (match.node ? match.node->result_name : "") << std::endl;
            if (!match.monotonicity || match.monotonicity->strict)
            {
                if (match.node && fixed_columns.contains(node))
                    fixed_key_columns.insert(match.node);
            }
        }

        enrichFixedColumns(sorting_key_dag, fixed_key_columns);
    }

    /// This is a result direction we will read from MergeTree
    ///  1 - in same order of keys,
    /// -1 - in reverse order of keys,
    ///  0 - usual read, don't apply optimization
    ///
    /// So far, 0 means any direction is possible. It is ok for constant prefix.
    int read_direction = 0;
    size_t next_description_column = 0;
    size_t next_sort_key = 0;

    bool can_optimize_virtual_row = true;

    struct MatchInfo
    {
        const ActionsDAG::Node * source = nullptr;
        const ActionsDAG::Node * fixed_column = nullptr;
        const MatchedTrees::Match * monotonic = nullptr;
    };

    std::vector<MatchInfo> match_infos;
    match_infos.reserve(description.size());

    while (next_description_column < description.size() && next_sort_key < sorting_key.column_names.size())
    {
        const auto & sorting_key_column = sorting_key.column_names[next_sort_key];
        int reverse_indicator = (!sorting_key.reverse_flags.empty() && sorting_key.reverse_flags[next_sort_key]) ? -1 : 1;
        const auto & sort_column_description = description[next_description_column];

        /// If required order depend on collation, it cannot be matched with primary key order.
        /// Because primary keys cannot have collations.
        if (sort_column_description.collator)
            break;

        /// Since sorting key columns are always sorted with
        // ASC NULLS LAST ("in order") or DESC NULLS FIRST ("reverse")
        /// supported only this direction, other cases are represented as nulls_direction==-1
        /// Also actual for floating point values NaN.
        const auto column_is_nullable = isNullableOrLowCardinalityNullable(sorting_key.data_types[next_sort_key])|| isFloat(*sorting_key.data_types[next_sort_key]);
        if (column_is_nullable && sort_column_description.nulls_direction == -1)
            break;

        /// Direction for current sort key.
        int current_direction = 0;
        bool strict_monotonic = true;

        const ActionsDAG::Node * sort_column_node = sorting_key_dag.tryFindInOutputs(sorting_key_column);
        /// This should not happen.
        if (!sort_column_node)
            break;

        if (!dag)
        {
            /// This is possible if there were no Expression or Filter steps in Plan.
            /// Example: SELECT * FROM tab ORDER BY a, b

            if (sort_column_node->type != ActionsDAG::ActionType::INPUT)
                break;

            if (sort_column_description.column_name != sorting_key_column)
                break;

            current_direction = sort_column_description.direction * reverse_indicator;

            //std::cerr << "====== (no dag) Found direct match" << std::endl;

            match_infos.push_back({.source = sort_column_node});
            ++next_description_column;
            ++next_sort_key;
        }
        else
        {
            const ActionsDAG::Node * sort_node = dag->tryFindInOutputs(sort_column_description.column_name);
             /// It is possible when e.g. sort by array joined column.
            if (!sort_node)
                break;

            const auto & match = matches[sort_node];

            //std::cerr << "====== Finding match for " << sort_column_node->result_name << ' ' << static_cast<const void *>(sort_column_node) << std::endl;

            if (match.node && match.node == sort_column_node)
            {
                //std::cerr << "====== Found direct match" << std::endl;

                /// We try to find the match first even if column is fixed. In this case, potentially more keys will match.
                /// Example: 'table (x Int32, y Int32) ORDER BY x + 1, y + 1'
                ///          'SELECT x, y FROM table WHERE x = 42 ORDER BY x + 1, y + 1'
                /// Here, 'x + 1' would be a fixed point. But it is reasonable to read-in-order.

                current_direction = sort_column_description.direction * reverse_indicator;
                if (match.monotonicity)
                {
                    current_direction *= match.monotonicity->direction;
                    strict_monotonic = match.monotonicity->strict;
                    match_infos.push_back({.source = sort_node, .monotonic = &match});
                }
                else
                    match_infos.push_back({.source = sort_node});

                ++next_description_column;
                ++next_sort_key;
            }
            else if (fixed_key_columns.contains(sort_column_node))
            {

                if (next_sort_key == 0)
                {
                    // Disable virtual row optimization.
                    // For example, when pk is (a,b), a = 1, order by b, virtual row should be
                    // disabled in the following case:
                    // 1st part (0, 100), (1, 2), (1, 3), (1, 4)
                    // 2nd part (0, 100), (1, 2), (1, 3), (1, 4).

                    can_optimize_virtual_row = false;
                }

                //std::cerr << "+++++++++ Found fixed key by match" << std::endl;
                ++next_sort_key;
            }
            else
            {
                //std::cerr << "====== Check for fixed const : " << bool(sort_node->column) << " fixed : " << fixed_columns.contains(sort_node) << std::endl;
                bool is_fixed_column = sort_node->column || fixed_columns.contains(sort_node);
                if (!is_fixed_column)
                    break;

                if (!sort_node->column)
                    /// Virtual row for fixed column from order by is not supported now.
                    /// TODO: we can do it for the simple case,
                    /// But it's better to remove fixed columns from ORDER BY completely, e.g:
                    /// WHERE x = 42 ORDER BY x, y    =>    WHERE x = 42 ORDER BY y
                    can_optimize_virtual_row = false;

                match_infos.push_back({.source = sort_node, .fixed_column = sort_node});
                order_key_prefix_descr.push_back(sort_column_description);
                ++next_description_column;
            }
        }

        /// read_direction == 0 means we can choose any global direction.
        /// current_direction == 0 means current key if fixed and any direction is possible for it.
        if (current_direction && read_direction && current_direction != read_direction)
            break;

        if (read_direction == 0)
            read_direction = current_direction;

        if (current_direction)
            order_key_prefix_descr.push_back(sort_column_description);

        if (current_direction && !strict_monotonic)
            break;
    }

    if (read_direction == 0 || order_key_prefix_descr.empty())
        return {};

    /// If the prefix description is used, we can't restore the full description from PK value.
    /// TODO: partial sort description can be used as well. Implement support later.
    if (order_key_prefix_descr.size() < description.size() || pk_column_names.size() < next_sort_key)
        can_optimize_virtual_row = false;

    auto order_info = std::make_shared<InputOrderInfo>(order_key_prefix_descr, next_sort_key, read_direction, limit);

    std::optional<ActionsDAG> virtual_row_conversion;
    if (can_optimize_virtual_row)
    {
        ActionsDAG virtual_row_dag;
        virtual_row_dag.getOutputs().reserve(match_infos.size());
        size_t next_pk_name = 0;
        for (const auto & info : match_infos)
        {
            const ActionsDAG::Node * output;
            if (info.fixed_column)
                output = &virtual_row_dag.addColumn({info.fixed_column->column, info.fixed_column->result_type, info.fixed_column->result_name});
            else
            {
                if (info.monotonic)
                    output = addMonotonicChain(virtual_row_dag, info.source, info.monotonic, pk_column_names[next_pk_name]);
                else
                {
                    output = &virtual_row_dag.addInput(pk_column_names[next_pk_name], info.source->result_type);
                    if (pk_column_names[next_pk_name] != info.source->result_name)
                        output = &virtual_row_dag.addAlias(*output, info.source->result_name);
                }

                ++next_pk_name;
            }

            virtual_row_dag.getOutputs().push_back(output);
        }
        virtual_row_conversion = std::move(virtual_row_dag);
    }

    return {std::move(order_info), std::move(virtual_row_conversion)};
}

/// We may need a few different sort descriptions here.
/// For example:
///
///   create table tab (a Int32, b Int32, c Int32, d Int32) engine = MergeTree order by (a, b, c);
///   select a, any(b), c, d from tab where b = 1 group by a, c, d order by c, d;
///
/// We would like to have:
/// (a, b, c) - a sort description for reading from table (it's into input_order)
/// (a, c) - a sort description for merging (an input of AggregatingInOrderTransfrom is sorted by this GROUP BY keys)
/// (a, c, d) - a group by sort description (an input of FinishAggregatingInOrderTransform is sorted by all GROUP BY keys)
///
/// The group by sort description is filled in optimizeAggregationInOrder now.
///
/// Sort description from input_order is not actually used. ReadFromMergeTree reads only PK prefix size.
/// We should remove it later.
struct InputOrder
{
    InputOrderInfoPtr input_order;
    SortDescription sort_description;
};

/// For the case when the order of keys is not important (GROUP BY / DISTINCT)
InputOrder buildInputOrderFromUnorderedKeys(
    const FixedColumns & fixed_columns,
    const std::optional<ActionsDAG> & dag,
    const Names & unordered_keys,
    const ActionsDAG & sorting_key_dag,
    const Names & sorting_key_columns)
{
    MatchedTrees::Matches matches;
    FixedColumns fixed_key_columns;

    /// For every column in PK find any match from GROUP BY key.
    using ReverseMatches = std::unordered_map<const ActionsDAG::Node *, MatchedTrees::Matches::const_iterator>;
    ReverseMatches reverse_matches;

    if (dag)
    {
        matches = matchTrees(sorting_key_dag.getOutputs(), *dag);

        for (const auto & [node, match] : matches)
        {
            if (!match.monotonicity || match.monotonicity->strict)
            {
                if (match.node && fixed_columns.contains(node))
                    fixed_key_columns.insert(match.node);
            }
        }

        enrichFixedColumns(sorting_key_dag, fixed_key_columns);

        for (const auto * output : dag->getOutputs())
        {
            auto it = matches.find(output);
            const MatchedTrees::Match * match = &it->second;
            if (match->node)
            {
                auto [jt, inserted] = reverse_matches.emplace(match->node, it);
                if (!inserted)
                {
                    /// Find the best match for PK node.
                    /// Direct match > strict monotonic > monotonic.
                    const MatchedTrees::Match * prev_match = &jt->second->second;
                    bool is_better = prev_match->monotonicity && !match->monotonicity;
                    if (!is_better)
                    {
                        bool both_monotionic = prev_match->monotonicity && match->monotonicity;
                        is_better = both_monotionic && match->monotonicity->strict && !prev_match->monotonicity->strict;
                    }

                    if (is_better)
                        jt->second = it;
                }
            }
        }
    }

    /// This is a result direction we will read from MergeTree
    ///  1 - in order,
    /// -1 - in reverse order,
    ///  0 - usual read, don't apply optimization
    ///
    /// So far, 0 means any direction is possible. It is ok for constant prefix.
    int read_direction = 0;
    size_t next_sort_key = 0;
    std::unordered_set<std::string_view> not_matched_keys(unordered_keys.begin(), unordered_keys.end());

    SortDescription sort_description;
    sort_description.reserve(unordered_keys.size());

    SortDescription order_key_prefix_descr;
    order_key_prefix_descr.reserve(sorting_key_columns.size());

    while (!not_matched_keys.empty() && next_sort_key < sorting_key_columns.size())
    {
        const auto & sorting_key_column = sorting_key_columns[next_sort_key];

        /// Direction for current sort key.
        int current_direction = 0;
        bool strict_monotonic = true;
        std::unordered_set<std::string_view>::iterator group_by_key_it;

        const ActionsDAG::Node * sort_column_node = sorting_key_dag.tryFindInOutputs(sorting_key_column);
        /// This should not happen.
        if (!sort_column_node)
            break;

        if (!dag)
        {
            /// This is possible if there were no Expression or Filter steps in Plan.
            /// Example: SELECT * FROM tab GROUP BY a, b

            if (sort_column_node->type != ActionsDAG::ActionType::INPUT)
                break;

            group_by_key_it = not_matched_keys.find(sorting_key_column);
            if (group_by_key_it == not_matched_keys.end())
                break;

            current_direction = 1;

            //std::cerr << "====== (no dag) Found direct match" << std::endl;
            ++next_sort_key;
        }
        else
        {
            const MatchedTrees::Match * match = nullptr;
            const ActionsDAG::Node * group_by_key_node = nullptr;
            if (const auto match_it = reverse_matches.find(sort_column_node); match_it != reverse_matches.end())
            {
                group_by_key_node = match_it->second->first;
                match = &match_it->second->second;
            }

            //std::cerr << "====== Finding match for " << sort_column_node->result_name << ' ' << static_cast<const void *>(sort_column_node) << std::endl;

            if (match && match->node)
                group_by_key_it = not_matched_keys.find(group_by_key_node->result_name);

            if (match && match->node && group_by_key_it != not_matched_keys.end())
            {
                //std::cerr << "====== Found direct match" << std::endl;

                current_direction = 1;
                if (match->monotonicity)
                {
                    current_direction *= match->monotonicity->direction;
                    strict_monotonic = match->monotonicity->strict;
                }

                ++next_sort_key;
            }
            else if (fixed_key_columns.contains(sort_column_node))
            {
                //std::cerr << "+++++++++ Found fixed key by match" << std::endl;
                ++next_sort_key;
            }
            else
                break;
        }

        /// read_direction == 0 means we can choose any global direction.
        /// current_direction == 0 means current key if fixed and any direction is possible for it.
        if (current_direction && read_direction && current_direction != read_direction)
            break;

        if (read_direction == 0 && current_direction != 0)
            read_direction = current_direction;

        if (current_direction)
        {
            /// Aggregation in order will always read in table order.
            /// Here, current_direction is a direction which will be applied to every key.
            /// Example:
            ///   CREATE TABLE t (x, y, z) ENGINE = MergeTree ORDER BY (x, y)
            ///   SELECT ... FROM t GROUP BY negate(y), negate(x), z
            /// Here, current_direction will be -1 cause negate() is negative montonic,
            /// Prefix sort description for reading will be (negate(y) DESC, negate(x) DESC),
            /// Sort description for GROUP BY will be (negate(y) DESC, negate(x) DESC, z).
            //std::cerr << "---- adding " << std::string(*group_by_key_it) << std::endl;
            sort_description.emplace_back(SortColumnDescription(std::string(*group_by_key_it), current_direction));
            order_key_prefix_descr.emplace_back(SortColumnDescription(std::string(*group_by_key_it), current_direction));
            not_matched_keys.erase(group_by_key_it);
        }
        else
        {
            /// If column is fixed, will read it in table order as well.
            //std::cerr << "---- adding " << sorting_key_column << std::endl;
            order_key_prefix_descr.emplace_back(SortColumnDescription(sorting_key_column, 1));
        }

        if (current_direction && !strict_monotonic)
            break;
    }

    if (read_direction == 0 || sort_description.empty())
        return {};

    // SortDescription sort_description_for_merging = group_by_sort_description;

    // for (const auto & key : not_matched_group_by_keys)
    //     group_by_sort_description.emplace_back(SortColumnDescription(std::string(key)));

    auto input_order = std::make_shared<InputOrderInfo>(order_key_prefix_descr, next_sort_key, /*read_direction*/ 1, /* limit */ 0);
    return { std::move(input_order), std::move(sort_description) }; // std::move(group_by_sort_description) };
}

SortingInputOrder buildInputOrderFromSortDescription(
    const ReadFromMergeTree * reading,
    const FixedColumns & fixed_columns,
    const std::optional<ActionsDAG> & dag,
    const SortDescription & description,
    size_t limit)
{
    const auto & sorting_key = reading->getStorageMetadata()->getSortingKey();
    const auto & pk_column_names = reading->getStorageMetadata()->getPrimaryKey().column_names;

    return buildInputOrderFromSortDescription(
        fixed_columns,
        dag, description,
        sorting_key,
        pk_column_names,
        limit);
}

SortingInputOrder buildInputOrderFromSortDescription(
    ReadFromMerge * merge,
    const FixedColumns & fixed_columns,
    const std::optional<ActionsDAG> & dag,
    const SortDescription & description,
    size_t limit)
{
    const auto & tables = merge->getSelectedTables();

    SortingInputOrder order_info;
    for (const auto & table : tables)
    {
        auto storage = std::get<StoragePtr>(table);
        auto metadata = storage->getInMemoryMetadataPtr();
        const auto & sorting_key = metadata->getSortingKey();
        // const auto & pk_column_names = metadata->getPrimaryKey().column_names;

        if (sorting_key.column_names.empty())
            return {};

        auto table_order_info = buildInputOrderFromSortDescription(
            fixed_columns,
            dag, description,
            sorting_key,
            {},
            limit);

        if (!table_order_info.input_order)
            return {};

        if (!order_info.input_order)
            order_info = std::move(table_order_info);
        else if (*order_info.input_order != *table_order_info.input_order)
            return {};
    }

    return order_info;
}

InputOrder buildInputOrderFromUnorderedKeys(
    ReadFromMergeTree * reading,
    const FixedColumns & fixed_columns,
    const std::optional<ActionsDAG> & dag,
    const Names & unordered_keys)
{
    const auto & sorting_key = reading->getStorageMetadata()->getSortingKey();
    const auto & sorting_key_columns = sorting_key.column_names;

    return buildInputOrderFromUnorderedKeys(
        fixed_columns,
        dag, unordered_keys,
        sorting_key.expression->getActionsDAG(), sorting_key_columns);
}

InputOrder buildInputOrderFromUnorderedKeys(
    ReadFromMerge * merge,
    const FixedColumns & fixed_columns,
    const std::optional<ActionsDAG> & dag,
    const Names & unordered_keys)
{
    const auto & tables = merge->getSelectedTables();

    InputOrder order_info;
    for (const auto & table : tables)
    {
        auto storage = std::get<StoragePtr>(table);
        const auto & sorting_key = storage->getInMemoryMetadataPtr()->getSortingKey();
        const auto & sorting_key_columns = sorting_key.column_names;

        if (sorting_key_columns.empty())
            return {};

        auto table_order_info = buildInputOrderFromUnorderedKeys(
            fixed_columns,
            dag, unordered_keys,
            sorting_key.expression->getActionsDAG(), sorting_key_columns);

        if (!table_order_info.input_order)
            return {};

        if (!order_info.input_order)
            order_info = table_order_info;
        else if (*order_info.input_order != *table_order_info.input_order)
            return {};
    }

    return order_info;
}

InputOrderInfoPtr buildInputOrderInfo(SortingStep & sorting, bool & apply_virtual_row, QueryPlan::Node & node)
{
    QueryPlan::Node * reading_node = findReadingStep(node, /*allow_existing_order=*/ false);
    if (!reading_node)
        return nullptr;

    const auto & description = sorting.getSortDescription();
    size_t limit = sorting.getLimit();

    std::optional<ActionsDAG> dag;
    FixedColumns fixed_columns;
    buildSortingDAG(node, dag, fixed_columns, limit);

    if (dag && !fixed_columns.empty())
        enrichFixedColumns(*dag, fixed_columns);

    if (auto * reading = typeid_cast<ReadFromMergeTree *>(reading_node->step.get()))
    {
        auto order_info = buildInputOrderFromSortDescription(
            reading,
            fixed_columns,
            dag, description,
            limit);

        if (order_info.input_order)
        {
            apply_virtual_row = order_info.virtual_row_conversion != std::nullopt;

            bool can_read = reading->requestReadingInOrder(
                order_info.input_order->used_prefix_of_sorting_key_size,
                order_info.input_order->direction,
                order_info.input_order->limit,
                std::move(order_info.virtual_row_conversion));

            if (!can_read)
                return nullptr;
        }

        return order_info.input_order;
    }
    if (auto * merge = typeid_cast<ReadFromMerge *>(reading_node->step.get()))
    {
        auto order_info = buildInputOrderFromSortDescription(
            merge,
            fixed_columns,
            dag, description,
            limit);

        if (order_info.input_order)
        {
            bool can_read = merge->requestReadingInOrder(order_info.input_order);
            if (!can_read)
                return nullptr;
        }

        return order_info.input_order;
    }

    return nullptr;
}

InputOrder buildInputOrderInfo(AggregatingStep & aggregating, QueryPlan::Node & node)
{
    QueryPlan::Node * reading_node = findReadingStep(node, /*allow_existing_order=*/ false);
    if (!reading_node)
        return {};

    const auto & keys = aggregating.getParams().keys;
    size_t limit = 0;

    std::optional<ActionsDAG> dag;
    FixedColumns fixed_columns;
    buildSortingDAG(node, dag, fixed_columns, limit);

    if (dag && !fixed_columns.empty())
        enrichFixedColumns(*dag, fixed_columns);

    if (auto * reading = typeid_cast<ReadFromMergeTree *>(reading_node->step.get()))
    {
        auto order_info = buildInputOrderFromUnorderedKeys(
            reading,
            fixed_columns,
            dag, keys);

        if (order_info.input_order)
        {
            bool can_read = reading->requestReadingInOrder(
                order_info.input_order->used_prefix_of_sorting_key_size,
                order_info.input_order->direction,
                order_info.input_order->limit,
                {});
            if (!can_read)
                return {};
        }

        return order_info;
    }
    if (auto * merge = typeid_cast<ReadFromMerge *>(reading_node->step.get()))
    {
        auto order_info = buildInputOrderFromUnorderedKeys(
            merge,
            fixed_columns,
            dag, keys);

        if (order_info.input_order)
        {
            bool can_read = merge->requestReadingInOrder(order_info.input_order);
            if (!can_read)
                return {};
        }

        return order_info;
    }

    return {};
}

bool canImproveOrderForDistinct(InputOrder & required_order, const InputOrderInfoPtr & existing_order)
{
    if (!required_order.input_order)
        return false;

    if (!existing_order)
        return true;

    /// We only allow improving existing order.
    /// In order to reuse previous order, applySorting.cpp is used.
    if (required_order.input_order->used_prefix_of_sorting_key_size <= existing_order->used_prefix_of_sorting_key_size)
        return false;

    if (existing_order && required_order.input_order->direction != existing_order->direction)
    {
        /// Take read direction from existing order.
        for (auto & column : required_order.sort_description)
            column.direction *= -1;

        required_order.input_order = std::make_shared<InputOrderInfo>(
            required_order.input_order->sort_description_for_merging,
            required_order.input_order->used_prefix_of_sorting_key_size,
            existing_order->direction,
            existing_order->limit);
    }

    return true;
}

InputOrder buildInputOrderInfo(DistinctStep & distinct, QueryPlan::Node & node)
{
    /// Here we allow improving existing in-order optimization.
    /// Example: SELECT DISTINCT a, b FROM t ORDER BY a; -- sorting key: a, b
    /// If read in order for ORDER BY is already applied, then output sort description will contain only column `a`,
    /// but we need columns `a, b`, applying read in order for distinct will still benefit `order by`
    QueryPlan::Node * reading_node = findReadingStep(node, /*allow_existing_order=*/ true);
    if (!reading_node)
        return {};

    const auto & keys = distinct.getColumnNames();
    size_t limit = 0;

    std::optional<ActionsDAG> dag;
    FixedColumns fixed_columns;
    buildSortingDAG(node, dag, fixed_columns, limit);

    if (dag && !fixed_columns.empty())
        enrichFixedColumns(*dag, fixed_columns);

    if (auto * reading = typeid_cast<ReadFromMergeTree *>(reading_node->step.get()))
    {
        auto order_info = buildInputOrderFromUnorderedKeys(
            reading,
            fixed_columns,
            dag, keys);

        if (!canImproveOrderForDistinct(order_info, reading->getInputOrder()))
            return {};

        if (!reading->requestReadingInOrder(
            order_info.input_order->used_prefix_of_sorting_key_size,
            order_info.input_order->direction,
            order_info.input_order->limit, {}))
            return {};

        return order_info;
    }
    if (auto * merge = typeid_cast<ReadFromMerge *>(reading_node->step.get()))
    {
        auto order_info = buildInputOrderFromUnorderedKeys(
            merge,
            fixed_columns,
            dag, keys);

        if (!canImproveOrderForDistinct(order_info, merge->getInputOrder()))
            return {};

        if (!merge->requestReadingInOrder(order_info.input_order))
            return {};

        return order_info;
    }

    return {};
}

bool readingFromParallelReplicas(const QueryPlan::Node * node)
{
    IQueryPlanStep * step = node->step.get();
    while (!node->children.empty())
    {
        step = node->children.front()->step.get();
        node = node->children.front();
    }

    return typeid_cast<const ReadFromParallelRemoteReplicasStep *>(step);
}

}

void optimizeReadInOrder(QueryPlan::Node & node, QueryPlan::Nodes & nodes)
{
    if (node.children.size() != 1)
        return;

    auto * sorting = typeid_cast<SortingStep *>(node.step.get());
    if (!sorting)
        return;

    //std::cerr << "---- optimizeReadInOrder found sorting" << std::endl;

    if (sorting->getType() != SortingStep::Type::Full)
        return;

    bool apply_virtual_row = false;

    if (typeid_cast<UnionStep *>(node.children.front()->step.get()))
    {
        auto & union_node = node.children.front();

        bool use_buffering = false;
        const SortDescription * max_sort_descr = nullptr;

        std::vector<InputOrderInfoPtr> infos;
        infos.reserve(node.children.size());

        for (const auto * child : union_node->children)
        {
            /// in case of parallel replicas
            /// avoid applying read-in-order optimization for local replica
            /// since it will lead to different parallel replicas modes
            /// between local and remote nodes
            if (readingFromParallelReplicas(child))
                return;
        }

        for (auto * child : union_node->children)
        {
            infos.push_back(buildInputOrderInfo(*sorting, apply_virtual_row, *child));

            if (infos.back())
            {
                if (!max_sort_descr || max_sort_descr->size() < infos.back()->sort_description_for_merging.size())
                    max_sort_descr = &infos.back()->sort_description_for_merging;

                use_buffering |= infos.back()->limit == 0;
            }
        }

        if (!max_sort_descr || max_sort_descr->empty())
            return;

        for (size_t i = 0; i < infos.size(); ++i)
        {
            const auto & info = infos[i];
            auto & child = union_node->children[i];

            QueryPlanStepPtr additional_sorting;

            if (!info)
            {
                auto limit = sorting->getLimit();
                /// If we have limit, it's better to sort up to full description and apply limit.
                /// We cannot sort up to partial read-in-order description with limit cause result set can be wrong.
                const auto & descr = limit ? sorting->getSortDescription() : *max_sort_descr;
                additional_sorting = std::make_unique<SortingStep>(
                    child->step->getOutputHeader(),
                    descr,
                    limit, /// TODO: support limit with ties
                    sorting->getSettings());
            }
            else if (info->sort_description_for_merging.size() < max_sort_descr->size())
            {
                additional_sorting = std::make_unique<SortingStep>(
                    child->step->getOutputHeader(),
                    info->sort_description_for_merging,
                    *max_sort_descr,
                    sorting->getSettings().max_block_size,
                    0); /// TODO: support limit with ties
            }

            if (additional_sorting)
            {
                auto & sort_node = nodes.emplace_back();
                sort_node.step = std::move(additional_sorting);
                sort_node.children.push_back(child);
                child = &sort_node;
            }
        }

        sorting->convertToFinishSorting(*max_sort_descr, use_buffering, false);
    }
    else if (auto order_info = buildInputOrderInfo(*sorting, apply_virtual_row, *node.children.front()))
    {
        /// Use buffering only if have filter or don't have limit.
        bool use_buffering = order_info->limit == 0;
        sorting->convertToFinishSorting(order_info->sort_description_for_merging, use_buffering, apply_virtual_row);
    }
}

void optimizeAggregationInOrder(QueryPlan::Node & node, QueryPlan::Nodes &)
{
    if (node.children.size() != 1)
        return;

    auto * aggregating = typeid_cast<AggregatingStep *>(node.step.get());
    if (!aggregating)
        return;

    if ((aggregating->inOrder() && !aggregating->explicitSortingRequired()) || aggregating->isGroupingSets())
        return;

    /// It just does not work, see 02515_projections_with_totals
    if (aggregating->getParams().overflow_row)
        return;

    /// TODO: maybe add support for UNION later.
    auto order_info = buildInputOrderInfo(*aggregating, *node.children.front());
    if (order_info.input_order)
    {
        std::unordered_set<std::string_view> used_keys;
        for (const auto & desc : order_info.sort_description)
            used_keys.insert(desc.column_name);

        /// Append other GROUP BY keys to sort description.
        SortDescription group_by_sort_description = order_info.sort_description;
        for (const auto & key : aggregating->getParams().keys)
            if (used_keys.emplace(key).second)
                group_by_sort_description.push_back(SortColumnDescription(std::string(key)));

        aggregating->applyOrder(std::move(order_info.sort_description), std::move(group_by_sort_description));
    }
}

void optimizeDistinctInOrder(QueryPlan::Node & node, QueryPlan::Nodes &)
{
    if (node.children.size() != 1)
        return;

    auto * distinct = typeid_cast<DistinctStep *>(node.step.get());
    if (!distinct)
        return;

    if (!distinct->isPreliminary())
        return;

    if (!distinct->getSortDescription().empty())
        return;

    auto order_info = buildInputOrderInfo(*distinct, *node.children.front());
    if (order_info.input_order)
        distinct->applyOrder(std::move(order_info.sort_description));
}

/// This optimization is obsolete and will be removed.
/// optimizeReadInOrder covers it.
size_t tryReuseStorageOrderingForWindowFunctions(QueryPlan::Node * parent_node, QueryPlan::Nodes & /*nodes*/, const Optimization::ExtraSettings & /*settings*/)
{
    /// Find the following sequence of steps, add InputOrderInfo and apply prefix sort description to
    /// SortingStep:
    /// WindowStep <- SortingStep <- [Expression] <- ReadFromMergeTree

    auto * window_node = parent_node;
    auto * window = typeid_cast<WindowStep *>(window_node->step.get());
    if (!window)
        return 0;
    if (window_node->children.size() != 1)
        return 0;

    auto * sorting_node = window_node->children.front();
    auto * sorting = typeid_cast<SortingStep *>(sorting_node->step.get());
    if (!sorting)
        return 0;
    if (sorting_node->children.size() != 1)
        return 0;

    auto * possible_read_from_merge_tree_node = sorting_node->children.front();

    if (typeid_cast<ExpressionStep *>(possible_read_from_merge_tree_node->step.get()))
    {
        if (possible_read_from_merge_tree_node->children.size() != 1)
            return 0;

        possible_read_from_merge_tree_node = possible_read_from_merge_tree_node->children.front();
    }

    auto * read_from_merge_tree = typeid_cast<ReadFromMergeTree *>(possible_read_from_merge_tree_node->step.get());
    if (!read_from_merge_tree)
    {
        return 0;
    }

    auto context = read_from_merge_tree->getContext();
    const auto & settings = context->getSettingsRef();
    if (!settings[Setting::optimize_read_in_window_order] || (settings[Setting::optimize_read_in_order] && settings[Setting::query_plan_read_in_order])
        || context->getSettingsRef()[Setting::allow_experimental_analyzer])
    {
        return 0;
    }

    const auto & query_info = read_from_merge_tree->getQueryInfo();
    const auto * select_query = query_info.query->as<ASTSelectQuery>();

    /// TODO: Analyzer syntax analyzer result
    if (!query_info.syntax_analyzer_result)
        return 0;

    ManyExpressionActions order_by_elements_actions;
    const auto & window_desc = window->getWindowDescription();

    for (const auto & actions_dag : window_desc.partition_by_actions)
    {
        order_by_elements_actions.emplace_back(
            std::make_shared<ExpressionActions>(actions_dag->clone(), ExpressionActionsSettings(context, CompileExpressions::yes)));
    }

    for (const auto & actions_dag : window_desc.order_by_actions)
    {
        order_by_elements_actions.emplace_back(
            std::make_shared<ExpressionActions>(actions_dag->clone(), ExpressionActionsSettings(context, CompileExpressions::yes)));
    }

    auto order_optimizer = std::make_shared<ReadInOrderOptimizer>(
            *select_query,
            order_by_elements_actions,
            window->getWindowDescription().full_sort_description,
            query_info.syntax_analyzer_result);

    /// If we don't have filtration, we can pushdown limit to reading stage for optimizations.
    UInt64 limit = (select_query->hasFiltration() || select_query->groupBy()) ? 0 : InterpreterSelectQuery::getLimitForSorting(*select_query, context);

    auto order_info = order_optimizer->getInputOrder(read_from_merge_tree->getStorageMetadata(), context, limit);

    if (order_info)
    {
        bool can_read = read_from_merge_tree->requestReadingInOrder(order_info->used_prefix_of_sorting_key_size, order_info->direction, order_info->limit, {});
        if (!can_read)
            return 0;
        sorting->convertToFinishSorting(order_info->sort_description_for_merging, false, false);
    }

    return 0;
}

}
