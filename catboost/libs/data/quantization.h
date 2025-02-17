#pragma once

#include "columns.h"
#include "data_provider.h"
#include "exclusive_feature_bundling.h"
#include "feature_grouping.h"
#include "catboost/libs/data/quantized_features_info.h"

#include <catboost/private/libs/index_range/index_range.h>
#include <catboost/libs/helpers/dynamic_iterator.h>
#include <catboost/libs/helpers/sparse_array.h>
#include <catboost/libs/helpers/restorable_rng.h>
#include <catboost/private/libs/options/data_processing_options.h>
#include <catboost/private/libs/options/catboost_options.h>

#include <library/threading/local_executor/local_executor.h>

#include <util/generic/array_ref.h>
#include <util/generic/maybe.h>
#include <util/generic/ylimits.h>
#include <util/system/types.h>

#include <functional>


namespace NPar {
    class TLocalExecutor;
}


namespace NCB {

    using TInitialBorders = TMaybe<TVector<TConstArrayRef<float>>>;

    struct TQuantizationOptions {
        bool CpuCompatibleFormat = true;
        bool GpuCompatibleFormat = true;
        ui64 CpuRamLimit = Max<ui64>();
        ui32 MaxSubsetSizeForBuildBordersAlgorithms = 200000;
        bool BundleExclusiveFeaturesForCpu = true;
        TExclusiveFeaturesBundlingOptions ExclusiveFeaturesBundlingOptions{};
        bool PackBinaryFeaturesForCpu = true;
        bool GroupFeaturesForCpu = false;
        TFeaturesGroupingOptions FeaturesGroupingOptions{};
        bool AllowWriteFiles = true;

        TMaybe<float> DefaultValueFractionToEnableSparseStorage = Nothing();
        ESparseArrayIndexingType SparseArrayIndexingType = ESparseArrayIndexingType::Indices;
    };

    /*
     * Used for optimization.
     * It is number of times more effective to iterate over dense data in incremental order instead of random
     *   access
     */
    struct TIncrementalDenseIndexing {
        // indices in SrcData for dense features, TFullSubset if there're no dense src data
        TFeaturesArraySubsetIndexing SrcSubsetIndexing;

        // positions in dst data when iterating over dense SrcData in SrcSubsetIndexing order
        TFeaturesArraySubsetIndexing DstIndexing;
    public:
        TIncrementalDenseIndexing(
            const TFeaturesArraySubsetIndexing& srcSubsetIndexing,
            bool hasDenseData,
            NPar::TLocalExecutor* localExecutor
        );
    };


    /*
     * return values dstMasks will contain pairs:
     *  pair.first is 64-documents block index
     *  pair.second is bit mask whether the corresponding quantized feature value bins are non-default
     */

    void GetQuantizedNonDefaultValuesMasks(
        const TFloatValuesHolder& floatValuesHolder,
        const TQuantizedFeaturesInfo& quantizedFeaturesInfo,
        const TFeaturesArraySubsetIndexing& incrementalIndexing,
        const TFeaturesArraySubsetInvertedIndexing& invertedIncrementalIndexing,
        TVector<std::pair<ui32, ui64>>* dstMasks,
        ui32* nonDefaultCount
    );

    void GetQuantizedNonDefaultValuesMasks(
        const THashedCatValuesHolder& catValuesHolder,
        const TQuantizedFeaturesInfo& quantizedFeaturesInfo,
        const TFeaturesArraySubsetIndexing& incrementalIndexing,
        const TFeaturesArraySubsetInvertedIndexing& invertedIncrementalIndexing,
        TVector<std::pair<ui32, ui64>>* dstMasks,
        ui32* nonDefaultCount
    );


    void CalcBordersAndNanMode(
        const TQuantizationOptions& options,
        TRawDataProviderPtr rawDataProvider,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
        TRestorableFastRng64* rand,
        NPar::TLocalExecutor* localExecutor
    );


    TQuantizedObjectsDataProviderPtr Quantize(
        const TQuantizationOptions& options,
        TRawObjectsDataProviderPtr rawObjectsDataProvider,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
        TRestorableFastRng64* rand,
        NPar::TLocalExecutor* localExecutor,
        const TInitialBorders& initialBorders = Nothing()
    );


    TQuantizedDataProviderPtr Quantize(
        const TQuantizationOptions& options,
        TRawDataProviderPtr rawDataProvider,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
        TRestorableFastRng64* rand,
        NPar::TLocalExecutor* localExecutor,
        const TInitialBorders& initialBorders = Nothing()
    );

    TQuantizedDataProviders Quantize(
        const TQuantizationOptions& options,
        const NCatboostOptions::TDataProcessingOptions& dataProcessingOptions,
        bool floatFeaturesAllowNansInTestOnly,
        TConstArrayRef<ui32> ignoredFeatures,
        TRawDataProviders rawDataProviders,
        TRestorableFastRng64* rand,
        NPar::TLocalExecutor* localExecutor
    );

    TQuantizedObjectsDataProviderPtr GetQuantizedObjectsData(
        const NCatboostOptions::TCatBoostOptions& params,
        TDataProviderPtr srcData,
        const TMaybe<TString>& bordersFile,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
        bool allowWriteFiles,
        NPar::TLocalExecutor* localExecutor,
        TRestorableFastRng64* rand,
        const TInitialBorders& initialBorders = Nothing()
    );

    TQuantizedObjectsDataProviderPtr ConstructQuantizedPoolFromRawPool(
        TDataProviderPtr pool,
        NJson::TJsonValue plainJsonParams,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo
    );

}
