#pragma once
#include <Processors/ISimpleTransform.h>
#include <QueryPipeline/SizeLimits.h>
#include <Core/ColumnNumbers.h>
#include <Interpreters/SetVariants.h>

namespace DB
{

class DistinctTransform : public ISimpleTransform
{
public:
    DistinctTransform(
        SharedHeader header_,
        const SizeLimits & set_size_limits_,
        UInt64 limit_hint_,
        const Names & columns_);

    String getName() const override { return "DistinctTransform"; }

protected:
    void transform(Chunk & chunk) override;

private:
    ColumnNumbers key_columns_pos;
    SetVariants data;
    Sizes key_sizes;
    const UInt64 limit_hint;

    /// Restrictions on the maximum size of the output data.
    SizeLimits set_size_limits;

    template <typename Method>
    void buildFilter(
        Method & method,
        const ColumnRawPtrs & key_columns,
        IColumn::Filter & filter,
        size_t rows,
        SetVariants & variants) const;
};

}
