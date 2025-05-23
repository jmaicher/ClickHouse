#include <gtest/gtest.h>

#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypesNumber.h>
#include <Disks/tests/gtest_disk.h>
#include <Formats/FormatFactory.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromString.h>
#include <Storages/StorageLog.h>
#include <Storages/SelectQueryInfo.h>
#include <Common/typeid_cast.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/tests/gtest_global_register.h>

#include <memory>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Processors/Executors/PushingPipelineExecutor.h>
#include <Processors/Executors/CompletedPipelineExecutor.h>
#include <Processors/Sinks/SinkToStorage.h>
#include <QueryPipeline/QueryPipeline.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/BuildQueryPipelineSettings.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <QueryPipeline/QueryPipelineBuilder.h>


DB::StoragePtr createStorage(DB::DiskPtr & disk)
{
    using namespace DB;

    NamesAndTypesList names_and_types;
    names_and_types.emplace_back("a", std::make_shared<DataTypeUInt64>());

    StoragePtr table = std::make_shared<StorageLog>(
        "Log", disk, "table/", StorageID("test", "test"), ColumnsDescription{names_and_types},
        ConstraintsDescription{}, String{}, LoadingStrictnessLevel::CREATE, getContext().context);

    table->startup();

    return table;
}

class StorageLogTest : public testing::Test
{
public:

    void SetUp() override
    {
        disk = createDisk();
        table = createStorage(disk);
    }

    void TearDown() override
    {
        table->flushAndShutdown();
        destroyDisk(disk);
    }

    const DB::DiskPtr & getDisk() { return disk; }
    DB::StoragePtr & getTable() { return table; }

private:
    DB::DiskPtr disk;
    DB::StoragePtr table;
};


// Returns data written to table in Values format.
std::string writeData(int rows, DB::StoragePtr & table, const DB::ContextPtr context)
{
    using namespace DB;
    auto metadata_snapshot = table->getInMemoryMetadataPtr();

    std::string data;

    Block block;

    {
        const auto & storage_columns = metadata_snapshot->getColumns();
        ColumnWithTypeAndName column;
        column.name = "a";
        column.type = storage_columns.getPhysical("a").type;
        auto col = column.type->createColumn();
        ColumnUInt64::Container & vec = typeid_cast<ColumnUInt64 &>(*col).getData();

        vec.resize(rows);
        for (size_t i = 0; i < rows; ++i)
        {
            vec[i] = i;
            if (i > 0)
                data += ",";
            data += "(" + std::to_string(i) + ")";
        }

        column.column = std::move(col);
        block.insert(column);
    }

    QueryPipeline pipeline(table->write({}, metadata_snapshot, context, /*async_insert=*/false));

    PushingPipelineExecutor executor(pipeline);
    executor.push(block);
    executor.finish();

    return data;
}

// Returns all table data in Values format.
std::string readData(DB::StoragePtr & table, const DB::ContextPtr context)
{
    using namespace DB;
    auto metadata_snapshot = table->getInMemoryMetadataPtr();
    auto storage_snapshot = table->getStorageSnapshot(metadata_snapshot, context);

    Names column_names;
    column_names.push_back("a");

    SelectQueryInfo query_info;
    QueryProcessingStage::Enum stage = table->getQueryProcessingStage(
        context, QueryProcessingStage::Complete, storage_snapshot, query_info);

    QueryPlan plan;
    table->read(plan, column_names, storage_snapshot, query_info, context, stage, 8192, 1);

    auto pipeline = QueryPipelineBuilder::getPipeline(std::move(*plan.buildQueryPipeline(
        QueryPlanOptimizationSettings(context), BuildQueryPipelineSettings(context))));

    Block sample;
    {
        ColumnWithTypeAndName col;
        col.type = std::make_shared<DataTypeUInt64>();
        col.name = "a";
        sample.insert(std::move(col));
    }

    tryRegisterFormats();

    WriteBufferFromOwnString out_buf;
    auto output = FormatFactory::instance().getOutputFormat("Values", out_buf, sample, context);
    pipeline.complete(output);

    Block data;

    CompletedPipelineExecutor executor(pipeline);
    executor.execute();
    // output->flush();

    out_buf.finalize();
    return out_buf.str();
}

TEST_F(StorageLogTest, testReadWrite)
{
    using namespace DB;
    const auto & context_holder = getContext();

    std::string data;

    // Write several chunks of data.
    data += writeData(10, this->getTable(), context_holder.context);
    data += ",";
    data += writeData(20, this->getTable(), context_holder.context);
    data += ",";
    data += writeData(10, this->getTable(), context_holder.context);

    ASSERT_EQ(data, readData(this->getTable(), context_holder.context));
}
