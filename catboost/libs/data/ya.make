LIBRARY()



SRCS(
    async_row_processor.cpp
    baseline.cpp
    borders_io.cpp
    cat_feature_perfect_hash.cpp
    cat_feature_perfect_hash_helper.cpp
    GLOBAL cb_dsv_loader.cpp
    columns.cpp
    data_provider.cpp
    data_provider_builders.cpp
    exclusive_feature_bundling.cpp
    external_columns.cpp
    feature_estimators.cpp
    feature_grouping.cpp
    feature_index.cpp
    features_layout.cpp
    feature_names_converter.cpp
    GLOBAL libsvm_loader.cpp
    load_data.cpp
    loader.cpp
    meta_info.cpp
    model_dataset_compatibility.cpp
    objects.cpp
    objects_grouping.cpp
    order.cpp
    packed_binary_features.cpp
    quantization.cpp
    quantized_features_info.cpp
    target.cpp
    unaligned_mem.cpp
    util.cpp
    visitor.cpp
    weights.cpp
)

PEERDIR(
    library/dbg_output
    library/object_factory
    library/pop_count
    library/string_utils/csv
    library/threading/future
    library/threading/local_executor

    catboost/libs/cat_feature
    catboost/libs/column_description
    catboost/private/libs/data_types
    catboost/private/libs/data_util
    catboost/private/libs/feature_estimator
    catboost/libs/helpers
    catboost/private/libs/index_range
    catboost/libs/logging
    catboost/libs/model
    catboost/private/libs/options
    catboost/private/libs/text_processing
    catboost/private/libs/quantization
    catboost/private/libs/quantization_schema
)

GENERATE_ENUM_SERIALIZATION(baseline.h)
GENERATE_ENUM_SERIALIZATION(order.h)
GENERATE_ENUM_SERIALIZATION(target.h)
GENERATE_ENUM_SERIALIZATION(visitor.h)

END()
