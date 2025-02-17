#include "quantization.h"

#include "borders_io.h"
#include "cat_feature_perfect_hash_helper.h"
#include "columns.h"
#include "external_columns.h"
#include "feature_names_converter.h"
#include "util.h"

#include <catboost/libs/helpers/array_subset.h>
#include <catboost/libs/helpers/compression.h>
#include <catboost/libs/helpers/double_array_iterator.h>
#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/helpers/mem_usage.h>
#include <catboost/libs/helpers/parallel_tasks.h>
#include <catboost/libs/helpers/sample.h>
#include <catboost/libs/helpers/resource_constrained_executor.h>
#include <catboost/libs/logging/logging.h>
#include <catboost/private/libs/labels/label_converter.h>
#include <catboost/private/libs/options/plain_options_helper.h>
#include <catboost/private/libs/options/system_options.h>
#include <catboost/private/libs/text_processing/text_column_builder.h>
#include <catboost/private/libs/quantization/utils.h>
#include <catboost/private/libs/quantization_schema/quantize.h>

#include <library/grid_creator/binarization.h>

#include <util/generic/algorithm.h>
#include <util/generic/cast.h>
#include <util/generic/maybe.h>
#include <util/generic/utility.h>
#include <util/generic/vector.h>
#include <util/generic/xrange.h>
#include <util/generic/ymath.h>
#include <util/random/shuffle.h>
#include <util/system/compiler.h>
#include <util/system/mem_info.h>

#include <limits>
#include <numeric>


namespace NCB {

    TIncrementalDenseIndexing::TIncrementalDenseIndexing(
        const TFeaturesArraySubsetIndexing& srcSubsetIndexing,
        bool hasDenseData,
        NPar::TLocalExecutor* localExecutor
    ) {
        if (hasDenseData && !HoldsAlternative<TFullSubset<ui32>>(srcSubsetIndexing)) {
            TVector<ui32> srcIndices;
            srcIndices.yresize(srcSubsetIndexing.Size());
            TArrayRef<ui32> srcIndicesRef = srcIndices;

            srcSubsetIndexing.ParallelForEach(
                [=] (ui32 objectIdx, ui32 srcObjectIdx) {
                    srcIndicesRef[objectIdx] = srcObjectIdx;
                },
                localExecutor
            );

            TVector<ui32> dstIndices;
            dstIndices.yresize(srcSubsetIndexing.Size());
            Iota(dstIndices.begin(), dstIndices.end(), ui32(0));

            TDoubleArrayIterator<ui32, ui32> beginIter{srcIndices.begin(), dstIndices.begin()};
            TDoubleArrayIterator<ui32, ui32> endIter{srcIndices.end(), dstIndices.end()};

            Sort(beginIter, endIter, [](auto lhs, auto rhs) { return lhs.first < rhs.first; });

            SrcSubsetIndexing = TFeaturesArraySubsetIndexing(std::move(srcIndices));
            DstIndexing = TFeaturesArraySubsetIndexing(std::move(dstIndices));
        } else {
            SrcSubsetIndexing = TFeaturesArraySubsetIndexing(TFullSubset<ui32>(srcSubsetIndexing.Size()));
            DstIndexing = TFeaturesArraySubsetIndexing(TFullSubset<ui32>(srcSubsetIndexing.Size()));
        }
    }


    static bool NeedToCalcBorders(
        const TFeaturesLayout& featuresLayoutForQuantization,
        const TQuantizedFeaturesInfo& quantizedFeaturesInfo
    ) {
        bool needToCalcBorders = false;
        featuresLayoutForQuantization.IterateOverAvailableFeatures<EFeatureType::Float>(
            [&] (TFloatFeatureIdx floatFeatureIdx) {
                if (!quantizedFeaturesInfo.HasBorders(floatFeatureIdx)) {
                    needToCalcBorders = true;
                }
            }
        );

        return needToCalcBorders;
    }

    struct TSubsetIndexingForBuildBorders {
        // for dense features, already composed with rawDataProvider's Subset, incremental
        TFeaturesArraySubsetIndexing ComposedSubset;

        // for sparse features
        TMaybe<TFeaturesArraySubsetInvertedIndexing> InvertedSubset;

    public:
        TSubsetIndexingForBuildBorders() = default;

        // composedSubset is not necessarily incremental
        TSubsetIndexingForBuildBorders(
            const TFeaturesArraySubsetIndexing& srcIndexing,
            const TFeaturesArraySubsetIndexing& subsetIndexing,
            NPar::TLocalExecutor* localExecutor
        ) {
            // non-incremental
            TFeaturesArraySubsetIndexing composedIndexing = Compose(srcIndexing, subsetIndexing);

            // convert to incremental
            if (HoldsAlternative<TFullSubset<ui32>>(composedIndexing)) {
                ComposedSubset = std::move(composedIndexing);
            } else {
                TVector<ui32> composedIndices;
                composedIndices.yresize(composedIndexing.Size());
                TArrayRef<ui32> composedIndicesRef = composedIndices;

                composedIndexing.ParallelForEach(
                    [=] (ui32 objectIdx, ui32 srcObjectIdx) {
                        composedIndicesRef[objectIdx] = srcObjectIdx;
                    },
                    localExecutor
                );

                Sort(composedIndices);

                ComposedSubset = TFeaturesArraySubsetIndexing(std::move(composedIndices));
            }
            InvertedSubset = GetInvertedIndexing(subsetIndexing, srcIndexing.Size(), localExecutor);
        }
    };

    // TODO(akhropov): maybe use different sample selection logic for sparse data
    static TSubsetIndexingForBuildBorders GetSubsetForBuildBorders(
        const TFeaturesArraySubsetIndexing& srcIndexing,
        const TFeaturesLayout& featuresLayoutForQuantization,
        const TQuantizedFeaturesInfo& quantizedFeaturesInfo,
        EObjectsOrder srcObjectsOrder,
        const TQuantizationOptions& options,
        TRestorableFastRng64* rand,
        NPar::TLocalExecutor* localExecutor
    ) {
        if (NeedToCalcBorders(featuresLayoutForQuantization, quantizedFeaturesInfo)) {
            const ui32 objectCount = srcIndexing.Size();
            const ui32 sampleSize = GetSampleSizeForBorderSelectionType(
                objectCount,
                /*TODO(kirillovs): iterate through all per feature binarization settings and select smallest
                 * sample size
                 */
                quantizedFeaturesInfo.GetFloatFeatureBinarization(Max<ui32>()).BorderSelectionType,
                options.MaxSubsetSizeForBuildBordersAlgorithms
            );
            TFeaturesArraySubsetIndexing subsetIndexing;
            if (sampleSize < objectCount) {
                if (srcObjectsOrder == EObjectsOrder::RandomShuffled) {
                    // just get first sampleSize elements
                    TVector<TSubsetBlock<ui32>> blocks = {TSubsetBlock<ui32>({0, sampleSize}, 0)};
                    subsetIndexing = TFeaturesArraySubsetIndexing(
                        TRangesSubset<ui32>(sampleSize, std::move(blocks))
                    );
                } else {
                    TIndexedSubset<ui32> randomShuffle = SampleIndices<ui32>(objectCount, sampleSize, rand);
                    subsetIndexing = TFeaturesArraySubsetIndexing(std::move(randomShuffle));
                }
            } else {
                subsetIndexing = TFeaturesArraySubsetIndexing(TFullSubset<ui32>(objectCount));
            }
            return TSubsetIndexingForBuildBorders(srcIndexing, subsetIndexing, localExecutor);
        } else {
            return TSubsetIndexingForBuildBorders();
        }
    }

    template <class T, EFeatureValuesType FeatureValuesType>
    static ui32 GetNonDefaultValuesCount(const TTypedFeatureValuesHolder<T, FeatureValuesType>& srcFeature) {
        using TDenseData = TPolymorphicArrayValuesHolder<T, FeatureValuesType>;
        using TSparseData = TSparsePolymorphicArrayValuesHolder<T, FeatureValuesType>;

        if (const auto* denseData = dynamic_cast<const TDenseData*>(&srcFeature)) {
            return denseData->GetSize();
        } else if (const auto* sparseData = dynamic_cast<const TSparseData*>(&srcFeature)) {
            return sparseData->GetData().GetNonDefaultSize();
        } else {
            CB_ENSURE_INTERNAL(false, "GetNonDefaultValuesCount: unsupported column type");
        }
    }


    static ui64 EstimateMemUsageForFloatFeature(
        const TFloatValuesHolder& srcFeature,
        const TQuantizedFeaturesInfo& quantizedFeaturesInfo,
        const TQuantizationOptions& options,
        bool doQuantization, // if false - only calc borders
        bool storeFeaturesDataAsExternalValuesHolder
    ) {
        ui64 result = 0;

        size_t borderCount;

        ui32 nonDefaultObjectCount = GetNonDefaultValuesCount(srcFeature);

        const TFloatFeatureIdx floatFeatureIdx
            = quantizedFeaturesInfo.GetPerTypeFeatureIdx<EFeatureType::Float>(srcFeature);

        if (!quantizedFeaturesInfo.HasBorders(floatFeatureIdx)) {
            // sampleSize is computed using defaultBinarizationSettings for now
            const auto& defaultBinarizationSettings
                = quantizedFeaturesInfo.GetFloatFeatureBinarization(Max<ui32>());

            const ui32 sampleSize = GetSampleSizeForBorderSelectionType(
                srcFeature.GetSize(),
                defaultBinarizationSettings.BorderSelectionType,
                options.MaxSubsetSizeForBuildBordersAlgorithms
            );

            ui32 nonDefaultSampleSize;
            TMaybe<TDefaultValue<float>> defaultValue;

            if (const auto* denseData = dynamic_cast<const TFloatArrayValuesHolder*>(&srcFeature)) {
                nonDefaultSampleSize = sampleSize;
            } else if (const auto* sparseData = dynamic_cast<const TFloatSparseValuesHolder*>(&srcFeature)) {
                const auto& sparseArray = sparseData->GetData();

                // random shuffle with select default and non-default values in this proportion
                nonDefaultSampleSize
                    = (sampleSize * sparseArray.GetNonDefaultSize()) / sparseArray.GetSize();
                const ui64 defaultSize = sparseArray.GetSize() - sparseArray.GetNonDefaultSize();
                if (defaultSize) {
                    defaultValue.ConstructInPlace(
                        sparseArray.GetDefaultValue(),
                        Max((sampleSize * defaultSize) / sparseArray.GetSize(), ui64(1))
                    );
                }
            } else {
                CB_ENSURE_INTERNAL(false, "EstimateMemUsageForFloatFeature: Unsupported column type");
            }

            result += sizeof(float) * nonDefaultSampleSize; // for copying to srcFeatureValuesForBuildBorders

            const auto& floatFeatureBinarizationSettings
                = quantizedFeaturesInfo.GetFloatFeatureBinarization(srcFeature.GetId());

            borderCount = floatFeatureBinarizationSettings.BorderCount.Get();

            result += NSplitSelection::CalcMemoryForFindBestSplit(
                SafeIntegerCast<int>(borderCount),
                nonDefaultSampleSize,
                defaultValue,
                floatFeatureBinarizationSettings.BorderSelectionType
            );
        } else {
            borderCount = quantizedFeaturesInfo.GetBorders(floatFeatureIdx).size();
        }

        if (doQuantization && !storeFeaturesDataAsExternalValuesHolder) {
            // for storing quantized data
            TIndexHelper<ui64> indexHelper(CalcHistogramWidthForBorders(borderCount));
            result += indexHelper.CompressedSize(nonDefaultObjectCount) * sizeof(ui64);
        }

        return result;
    }


    static void CalcQuantizationAndNanMode(
        const TFloatValuesHolder& srcFeature,
        const TSubsetIndexingForBuildBorders& subsetIndexingForBuildBorders,
        const TQuantizedFeaturesInfo& quantizedFeaturesInfo,
        const TMaybe<TVector<float>>& initialBorders,
        TMaybe<float> quantizedDefaultBinFraction,
        ENanMode* nanMode,
        NSplitSelection::TQuantization* quantization
    ) {
        const auto& binarizationOptions = quantizedFeaturesInfo.GetFloatFeatureBinarization(srcFeature.GetId());

        Y_VERIFY(binarizationOptions.BorderCount > 0);

        const ui32 sampleCount = subsetIndexingForBuildBorders.ComposedSubset.Size();

        // featureValues.Values will not contain nans
        NSplitSelection::TFeatureValues featureValues{TVector<float>()};

        bool hasNans = false;

        auto processNonDefaultValue = [&] (ui32 /*idx*/, float value) {
            if (IsNan(value)) {
                hasNans = true;
            } else {
                featureValues.Values.push_back(value);
            }
        };

        if (const auto* denseSrcFeature = dynamic_cast<const TFloatArrayValuesHolder*>(&srcFeature)) {
            ITypedArraySubsetPtr<float> srcFeatureData = denseSrcFeature->GetData();

            ITypedArraySubsetPtr<float> srcDataForBuildBorders = srcFeatureData->CloneWithNewSubsetIndexing(
                &subsetIndexingForBuildBorders.ComposedSubset
            );

            // does not contain nans
            featureValues.Values.reserve(sampleCount);

            srcDataForBuildBorders->ForEach(processNonDefaultValue);
        } else if (const auto* sparseSrcFeature = dynamic_cast<const TFloatSparseValuesHolder*>(&srcFeature)) {
            const TConstPolymorphicValuesSparseArray<float, ui32>& sparseData = sparseSrcFeature->GetData();

            ui32 nonDefaultValuesInSampleCount = 0;

            if (const auto* invertedIndexedSubset
                    = GetIf<TInvertedIndexedSubset<ui32>>(&*subsetIndexingForBuildBorders.InvertedSubset))
            {
                TConstArrayRef<ui32> invertedMapping = invertedIndexedSubset->GetMapping();
                sparseData.ForEachNonDefault(
                    [&, invertedMapping] (ui32 idx, float value) {
                        if (invertedMapping[idx] != TInvertedIndexedSubset<ui32>::NOT_PRESENT) {
                            processNonDefaultValue(idx, value);
                            ++nonDefaultValuesInSampleCount;
                        }
                    }
                );
            } else { // TFullSubset
                sparseData.ForEachNonDefault(
                    [&] (ui32 idx, float value) {
                        processNonDefaultValue(idx, value);
                    }
                );
                nonDefaultValuesInSampleCount = sparseData.GetNonDefaultSize();
            }

            const ui32 defaultValuesSampleCount = sampleCount - nonDefaultValuesInSampleCount;
            if (defaultValuesSampleCount) {
                if (IsNan(sparseData.GetDefaultValue())) {
                    hasNans = true;
                } else {
                    featureValues.DefaultValue.ConstructInPlace(
                        sparseData.GetDefaultValue(),
                        defaultValuesSampleCount
                    );
                }
            }
        } else {
            CB_ENSURE_INTERNAL(false, "CalcQuantizationAndNanMode: Unsupported column type");
        }

        CB_ENSURE(
            (binarizationOptions.NanMode != ENanMode::Forbidden) ||
            !hasNans,
            "Feature #" << srcFeature.GetId() << ": There are nan factors and nan values for "
            " float features are not allowed. Set nan_mode != Forbidden."
        );

        int nonNanValuesBorderCount = binarizationOptions.BorderCount;
        if (hasNans) {
            *nanMode = binarizationOptions.NanMode;
            --nonNanValuesBorderCount;
        } else {
            *nanMode = ENanMode::Forbidden;
        }

        if (nonNanValuesBorderCount > 0) {
            *quantization = NSplitSelection::BestSplit(
                std::move(featureValues),
                /*featureValuesMayContainNans*/ false,
                nonNanValuesBorderCount,
                binarizationOptions.BorderSelectionType,
                quantizedDefaultBinFraction,
                initialBorders
            );
        }

        if (*nanMode == ENanMode::Min) {
            quantization->Borders.insert(quantization->Borders.begin(), std::numeric_limits<float>::lowest());
        } else if (*nanMode == ENanMode::Max) {
            quantization->Borders.push_back(std::numeric_limits<float>::max());
        }
    }


    template <class T, EFeatureValuesType FeatureValuesType>
    class TIsNonDefault {
    public:
        Y_FORCE_INLINE bool IsNonDefault(T srcValue) const;
    };

    template <>
    class TIsNonDefault<float, EFeatureValuesType::Float> {
    public:
        TIsNonDefault(const TQuantizedFeaturesInfo& quantizedFeaturesInfo, ui32 flatFeatureIdx)
            : FlatFeatureIdx(flatFeatureIdx)
            , NanMode(ENanMode::Forbidden) // properly inited below
            , AllowNans(false) // properly inited below
            , DefaultBinLowerBorder(0.0f) // properly inited below
            , DefaultBinUpperBorder(0.0f) // properly inited below
        {
            const TFloatFeatureIdx floatFeatureIdx
                = quantizedFeaturesInfo.GetFeaturesLayout()->GetInternalFeatureIdx<EFeatureType::Float>(
                    FlatFeatureIdx
                );

            NanMode = quantizedFeaturesInfo.GetNanMode(floatFeatureIdx);
            AllowNans = (NanMode != ENanMode::Forbidden) ||
                quantizedFeaturesInfo.GetFloatFeaturesAllowNansInTestOnly();

            const NSplitSelection::TQuantization& quantization
                = quantizedFeaturesInfo.GetQuantization(floatFeatureIdx);

            DefaultBinLowerBorder = std::numeric_limits<float>::lowest();
            DefaultBinUpperBorder = quantization.Borders.front();
        }

        Y_FORCE_INLINE bool IsNonDefault(float srcValue) const {
            if (IsNan(srcValue)) {
                CB_ENSURE(
                    AllowNans,
                    "There are NaNs in test dataset (feature number "
                    << FlatFeatureIdx << ") but there were no NaNs in learn dataset"
                );
                if (NanMode == ENanMode::Max) {
                    return true;
                }
            } else if ((srcValue <= DefaultBinLowerBorder) || (srcValue > DefaultBinUpperBorder)) {
                return true;
            }
            return false;
        }

    private:
        ui32 FlatFeatureIdx;
        ENanMode NanMode;
        bool AllowNans;
        float DefaultBinLowerBorder;
        float DefaultBinUpperBorder;
    };


    template <>
    class TIsNonDefault<ui32, EFeatureValuesType::HashedCategorical> {
    public:
        TIsNonDefault(const TQuantizedFeaturesInfo& quantizedFeaturesInfo, ui32 flatFeatureIdx) {
            const TCatFeatureIdx catFeatureIdx
                = quantizedFeaturesInfo.GetFeaturesLayout()
                    ->GetInternalFeatureIdx<EFeatureType::Categorical>(flatFeatureIdx);

            const auto& perfectHash = quantizedFeaturesInfo.GetCategoricalFeaturesPerfectHash(catFeatureIdx);

            bool fromDefaultMap = false;
            if (perfectHash.DefaultMap) {
                if (perfectHash.DefaultMap->DstValueWithCount.Value == 0) {
                    fromDefaultMap = true;
                    HashedCatValueMappedTo0 = perfectHash.DefaultMap->SrcValue;
                }
            }
            if (!fromDefaultMap) {
                for (const auto& [hashedCatValue, valueAndCount] : perfectHash.Map) {
                    if (valueAndCount.Value == 0) {
                        HashedCatValueMappedTo0 = hashedCatValue;
                        break;
                    }
                }
            }
        }

        Y_FORCE_INLINE bool IsNonDefault(ui32 srcValue) const {
            return srcValue != HashedCatValueMappedTo0;
        }

    private:
        ui32 HashedCatValueMappedTo0 = 0;
    };


    template <class T, EFeatureValuesType FeatureValuesType>
    class TGetQuantizedNonDefaultValuesMasks {
    private:
        constexpr static ui32 BLOCK_SIZE = sizeof(ui64) * CHAR_BIT;

    public:
        TGetQuantizedNonDefaultValuesMasks(
            TIsNonDefault<T, FeatureValuesType>&& isNonDefaultFunctor,
            TVector<std::pair<ui32, ui64>>* masks,
            ui32* nonDefaultCount)
            : IsNonDefaultFunctor(std::move(isNonDefaultFunctor))
            , DstMasks(masks)
            , DstNonDefaultCount(nonDefaultCount)
        {}

        inline void UpdateInIncrementalOrder(
            ui32 idx,
            ui32* currentBlockIdx,
            ui64* currentBlockMask
        ) const {
            ++(*DstNonDefaultCount);

            const ui32 blockIdx = idx / BLOCK_SIZE;
            const ui64 bitMask = ui64(1) << (idx % BLOCK_SIZE);
            if (blockIdx == *currentBlockIdx) {
                *currentBlockMask |= bitMask;
            } else {
                if (*currentBlockIdx != Max<ui32>()) {
                    DstMasks->push_back(std::pair<ui32, ui64>(*currentBlockIdx, *currentBlockMask));
                }
                *currentBlockIdx = blockIdx;
                *currentBlockMask = bitMask;
            }
        }

        void ProcessDenseColumn(
            const TPolymorphicArrayValuesHolder<T, FeatureValuesType>& denseColumn,
            const TFeaturesArraySubsetIndexing& incrementalIndexing
        ) const {
            ui32 currentBlockIdx = Max<ui32>();
            ui64 currentBlockMask = 0;

            denseColumn.GetData()->CloneWithNewSubsetIndexing(&incrementalIndexing)->ForEach(
                [&] (ui32 idx, T srcValue) {
                    if (IsNonDefaultFunctor.IsNonDefault(srcValue)) {
                        UpdateInIncrementalOrder(idx, &currentBlockIdx, &currentBlockMask);
                    }
                }
            );
            if (currentBlockIdx != Max<ui32>()) {
                DstMasks->push_back(std::pair<ui32, ui64>(currentBlockIdx, currentBlockMask));
            }
        }

        void NonDefaultIndicesToMasks(TVector<ui32>&& nonDefaultIndices) const {
            Sort(nonDefaultIndices);

            ui32 currentBlockIdx = Max<ui32>();
            ui64 currentBlockMask = 0;

            for (auto idx : nonDefaultIndices) {
                UpdateInIncrementalOrder(idx, &currentBlockIdx, &currentBlockMask);
            }

            if (currentBlockIdx != Max<ui32>()) {
                DstMasks->push_back(std::pair<ui32, ui64>(currentBlockIdx, currentBlockMask));
            }
        }

        void ProcessSparseColumnWithSrcDefaultEqualToDstDefault(
            const TConstPolymorphicValuesSparseArray<T, ui32>& sparseArray,
            const TFeaturesArraySubsetInvertedIndexing& incrementalInvertedIndexing
        ) const {
            if (const TInvertedIndexedSubset<ui32>* invertedIndexedSubset
                    = GetIf<TInvertedIndexedSubset<ui32>>(&incrementalInvertedIndexing))
            {
                TConstArrayRef<ui32> invertedIndexedSubsetArray = invertedIndexedSubset->GetMapping();
                TVector<ui32> nonDefaultIndices;
                nonDefaultIndices.reserve(sparseArray.GetNonDefaultSize());

                sparseArray.ForEachNonDefault(
                    [&] (ui32 nonDefaultIdx, T srcNonDefaultValue) {
                        if (IsNonDefaultFunctor.IsNonDefault(srcNonDefaultValue)) {
                            nonDefaultIndices.push_back(invertedIndexedSubsetArray[nonDefaultIdx]);
                        }
                    }
                );

                NonDefaultIndicesToMasks(std::move(nonDefaultIndices));
            } else {
                // TFullSubset

                ui32 currentBlockIdx = Max<ui32>();
                ui64 currentBlockMask = 0;

                sparseArray.ForEachNonDefault(
                    [&] (ui32 nonDefaultIdx, T srcNonDefaultValue) {
                        if (IsNonDefaultFunctor.IsNonDefault(srcNonDefaultValue)) {
                            UpdateInIncrementalOrder(nonDefaultIdx, &currentBlockIdx, &currentBlockMask);
                        }
                    }
                );

                if (currentBlockIdx != Max<ui32>()) {
                    DstMasks->push_back(std::pair<ui32, ui64>(currentBlockIdx, currentBlockMask));
                }
            }
        }

        void ProcessSparseColumnWithSrcDefaultNotEqualToDstDefault(
            const TConstPolymorphicValuesSparseArray<T, ui32>& sparseArray,
            const TFeaturesArraySubsetInvertedIndexing& incrementalInvertedIndexing
        ) const {
            if (const TInvertedIndexedSubset<ui32>* invertedIndexedSubset
                    = GetIf<TInvertedIndexedSubset<ui32>>(&incrementalInvertedIndexing))
            {
                TConstArrayRef<ui32> invertedIndexedSubsetArray = invertedIndexedSubset->GetMapping();
                TVector<ui32> nonDefaultIndices;
                nonDefaultIndices.reserve(sparseArray.GetSize());

                ui32 idx = 0;
                sparseArray.ForEachNonDefault(
                    [&] (ui32 nonDefaultIdx, T srcNonDefaultValue) {
                        for (; idx < nonDefaultIdx; ++idx) {
                            nonDefaultIndices.push_back(invertedIndexedSubsetArray[idx]);
                        }
                        if (IsNonDefaultFunctor.IsNonDefault(srcNonDefaultValue)) {
                            nonDefaultIndices.push_back(invertedIndexedSubsetArray[nonDefaultIdx]);
                        }
                        ++idx;
                    }
                );
                for (; idx < sparseArray.GetSize(); ++idx) {
                    nonDefaultIndices.push_back(invertedIndexedSubsetArray[idx]);
                }

                NonDefaultIndicesToMasks(std::move(nonDefaultIndices));
            } else {
                // TFullSubset

                ui32 currentBlockIdx = Max<ui32>();
                ui64 currentBlockMask = 0;

                ui32 idx = 0;

                sparseArray.ForEachNonDefault(
                    [&] (ui32 nonDefaultIdx, T srcNonDefaultValue) {
                        for (; idx < nonDefaultIdx; ++idx) {
                            UpdateInIncrementalOrder(idx, &currentBlockIdx, &currentBlockMask);
                        }
                        if (IsNonDefaultFunctor.IsNonDefault(srcNonDefaultValue)) {
                            UpdateInIncrementalOrder(nonDefaultIdx, &currentBlockIdx, &currentBlockMask);
                        }
                        ++idx;
                    }
                );
                for (; idx < sparseArray.GetSize(); ++idx) {
                    UpdateInIncrementalOrder(idx, &currentBlockIdx, &currentBlockMask);
                }

                if (currentBlockIdx != Max<ui32>()) {
                    DstMasks->push_back(std::pair<ui32, ui64>(currentBlockIdx, currentBlockMask));
                }
            }
        }

        void ProcessSparseColumn(
            const TSparsePolymorphicArrayValuesHolder<T, FeatureValuesType>& sparseColumn,
            const TFeaturesArraySubsetInvertedIndexing& incrementalInvertedIndexing
        ) const {
            const auto& sparseArray = sparseColumn.GetData();
            if (IsNonDefaultFunctor.IsNonDefault(sparseArray.GetDefaultValue())) {
                ProcessSparseColumnWithSrcDefaultNotEqualToDstDefault(
                    sparseArray,
                    incrementalInvertedIndexing
                );
            } else {
                ProcessSparseColumnWithSrcDefaultEqualToDstDefault(
                    sparseArray,
                    incrementalInvertedIndexing
                );
            }
        }

        void ProcessColumn(
            const TTypedFeatureValuesHolder<T, FeatureValuesType>& column,
            const TFeaturesArraySubsetIndexing& incrementalIndexing,
            const TFeaturesArraySubsetInvertedIndexing& invertedIncrementalIndexing
        ) const {
            if (const auto* denseColumn
                    = dynamic_cast<const TPolymorphicArrayValuesHolder<T, FeatureValuesType>*>(&column))
            {
                ProcessDenseColumn(*denseColumn, incrementalIndexing);
            } else if (const auto* sparseColumn
                           = dynamic_cast<const TSparsePolymorphicArrayValuesHolder<T, FeatureValuesType>*>(&column))
            {
                ProcessSparseColumn(*sparseColumn, invertedIncrementalIndexing);
            } else {
                CB_ENSURE(false, "Unsupported column type");
            }
        }

    private:
        TIsNonDefault<T, FeatureValuesType> IsNonDefaultFunctor;

        TVector<std::pair<ui32, ui64>>* DstMasks;
        ui32* DstNonDefaultCount;
    };


    /* GetQuantizedNonDefaultValuesMasks are copy-paste but this way we avoid exposing implementation
     *   in header
     */

    void GetQuantizedNonDefaultValuesMasks(
        const TFloatValuesHolder& floatValuesHolder,
        const TQuantizedFeaturesInfo& quantizedFeaturesInfo,
        const TFeaturesArraySubsetIndexing& incrementalIndexing,
        const TFeaturesArraySubsetInvertedIndexing& invertedIncrementalIndexing,
        TVector<std::pair<ui32, ui64>>* masks,
        ui32* nonDefaultCount
    ) {
        TGetQuantizedNonDefaultValuesMasks processor(
            TIsNonDefault<float, EFeatureValuesType::Float>(
                quantizedFeaturesInfo,
                floatValuesHolder.GetId()
            ),
            masks,
            nonDefaultCount
        );

        processor.ProcessColumn(floatValuesHolder, incrementalIndexing, invertedIncrementalIndexing);
    }

    void GetQuantizedNonDefaultValuesMasks(
        const THashedCatValuesHolder& catValuesHolder,
        const TQuantizedFeaturesInfo& quantizedFeaturesInfo,
        const TFeaturesArraySubsetIndexing& incrementalIndexing,
        const TFeaturesArraySubsetInvertedIndexing& invertedIncrementalIndexing,
        TVector<std::pair<ui32, ui64>>* masks,
        ui32* nonDefaultCount
    ) {
        TGetQuantizedNonDefaultValuesMasks processor(
            TIsNonDefault<ui32, EFeatureValuesType::HashedCategorical>(
                quantizedFeaturesInfo,
                catValuesHolder.GetId()
            ),
            masks,
            nonDefaultCount
        );

        processor.ProcessColumn(catValuesHolder, incrementalIndexing, invertedIncrementalIndexing);
    }


    template <
        class IQuantizedValuesHolder,
        class TExternalValuesHolder,
        class TExternalSparseValuesHolder,
        class TSrc,
        EFeatureValuesType SrcFeatureValuesType>
    static THolder<IQuantizedValuesHolder> MakeExternalValuesHolder(
        const TTypedFeatureValuesHolder<TSrc, SrcFeatureValuesType>& srcFeature,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo
    ) {
        using TDenseSrcFeature = TPolymorphicArrayValuesHolder<TSrc, SrcFeatureValuesType>;
        using TSparseSrcFeature = TSparsePolymorphicArrayValuesHolder<TSrc, SrcFeatureValuesType>;

        if (const auto* denseSrcFeature = dynamic_cast<const TDenseSrcFeature*>(&srcFeature)){
            return MakeHolder<TExternalValuesHolder>(
                denseSrcFeature->GetId(),
                denseSrcFeature->GetData(),
                quantizedFeaturesInfo
            );
        } else if (const auto* sparseSrcFeature = dynamic_cast<const TSparseSrcFeature*>(&srcFeature)) {
            return MakeHolder<TExternalSparseValuesHolder>(
                sparseSrcFeature->GetId(),
                sparseSrcFeature->GetData(),
                quantizedFeaturesInfo
            );
        } else {
            CB_ENSURE_INTERNAL(false, "MakeExternalValuesHolder: unsupported src feature type");
        }
    }

    template <class TSrcValue, EFeatureValuesType FeatureValuesType>
    class TValueQuantizer {
    public:
        ui32 GetDstBitsPerKey() const;
        Y_FORCE_INLINE ui32 Quantize(TSrcValue srcValue) const;
        TMaybe<ui32> GetDefaultBin() const;
    };

    template <>
    class TValueQuantizer<float, EFeatureValuesType::Float> {
    public:
        TValueQuantizer(const TQuantizedFeaturesInfo& quantizedFeaturesInfo, ui32 flatFeatureIdx)
            : FlatFeatureIdx(flatFeatureIdx)
            , NanMode(ENanMode::Forbidden) // properly inited below
            , AllowNans(false) // properly inited below
        {
            const TFloatFeatureIdx floatFeatureIdx
                = quantizedFeaturesInfo.GetFeaturesLayout()->GetInternalFeatureIdx<EFeatureType::Float>(
                    FlatFeatureIdx
                );

            {
                // because features can be quantized while quantizedFeaturesInfo is still updating
                TReadGuard guard(quantizedFeaturesInfo.GetRWMutex());

                NanMode = quantizedFeaturesInfo.GetNanMode(floatFeatureIdx);
                AllowNans = (NanMode != ENanMode::Forbidden) ||
                    quantizedFeaturesInfo.GetFloatFeaturesAllowNansInTestOnly();
                const auto& quantization = quantizedFeaturesInfo.GetQuantization(floatFeatureIdx);
                Borders = quantization.Borders;
                if (quantization.DefaultQuantizedBin) {
                    DefaultBin = quantization.DefaultQuantizedBin->Idx;
                }
            }
        }

        TValueQuantizer(const TQuantizedFeaturesInfo& quantizedFeaturesInfo, TFloatFeatureIdx floatFeatureIdx)
            : TValueQuantizer(
                quantizedFeaturesInfo,
                quantizedFeaturesInfo.GetFeaturesLayout()->GetExternalFeatureIdx(
                    *floatFeatureIdx,
                    EFeatureType::Float
                )
              )
        {}

        ui32 GetDstBitsPerKey() const {
            return CalcHistogramWidthForBorders(Borders.size());
        }

        Y_FORCE_INLINE ui32 Quantize(float srcValue) const {
            return NCB::Quantize<ui32>(FlatFeatureIdx, AllowNans, NanMode, Borders, srcValue);
        }

        TMaybe<ui32> GetDefaultBin() const {
            return DefaultBin;
        }

    private:
        ui32 FlatFeatureIdx;
        ENanMode NanMode;
        bool AllowNans;
        TConstArrayRef<float> Borders;

        TMaybe<ui32> DefaultBin;
    };

    template <>
    class TValueQuantizer<ui32, EFeatureValuesType::HashedCategorical> {
    public:
        TValueQuantizer(const TQuantizedFeaturesInfo& quantizedFeaturesInfo, ui32 flatFeatureIdx) {
            const TCatFeatureIdx catFeatureIdx
                = quantizedFeaturesInfo.GetFeaturesLayout()->GetInternalFeatureIdx<EFeatureType::Categorical>(
                    flatFeatureIdx
                );

            {
                // because features can be quantized while quantizedFeaturesInfo is still updating
                TReadGuard guard(quantizedFeaturesInfo.GetRWMutex());

                PerfectHash = &quantizedFeaturesInfo.GetCategoricalFeaturesPerfectHash(catFeatureIdx);
            }
        }

        TValueQuantizer(const TQuantizedFeaturesInfo& quantizedFeaturesInfo, TCatFeatureIdx catFeatureIdx)
            : TValueQuantizer(
                quantizedFeaturesInfo,
                quantizedFeaturesInfo.GetFeaturesLayout()->GetExternalFeatureIdx(
                    *catFeatureIdx,
                    EFeatureType::Categorical
                )
              )
        {}

        ui32 GetDstBitsPerKey() const {
            // TODO(akhropov): support other bitsPerKey. MLTOOLS-2425
            return 32;
        }

        Y_FORCE_INLINE ui32 Quantize(ui32 srcValue) const {
            return PerfectHash->Find(srcValue)->Value;
        }

        TMaybe<ui32> GetDefaultBin() const {
            if (PerfectHash->DefaultMap.Defined()) {
                return PerfectHash->DefaultMap->DstValueWithCount.Value;
            } else {
                return Nothing();
            }
        }

    private:
        const TCatFeaturePerfectHash* PerfectHash = nullptr;
    };


    template <
        class TStoredDstValue,
        class TDst,
        EFeatureValuesType DstFeatureValuesType,
        class TSrc,
        EFeatureValuesType SrcFeatureValuesType>
    static void MakeQuantizedColumnWithDefaultBin(
        const TTypedFeatureValuesHolder<TSrc, SrcFeatureValuesType>& srcFeature,
        TValueQuantizer<TSrc, SrcFeatureValuesType> valueQuantizer,
        ESparseArrayIndexingType sparseArrayIndexingType,

        // pass as parameter to enable type deduction
        THolder<TTypedFeatureValuesHolder<TDst, DstFeatureValuesType>>* dstFeature
    ) {
        using TDenseSrcData = TPolymorphicArrayValuesHolder<TSrc, SrcFeatureValuesType>;
        using TSparseSrcData = TSparsePolymorphicArrayValuesHolder<TSrc, SrcFeatureValuesType>;

        Y_ASSERT(valueQuantizer.GetDstBitsPerKey() == sizeof(TStoredDstValue) * CHAR_BIT);

        ui32 defaultQuantizedBin = *valueQuantizer.GetDefaultBin();

        TSparseArrayIndexingBuilderPtr<ui32> indexingBuilder
            = CreateSparseArrayIndexingBuilder<ui32>(sparseArrayIndexingType);

        constexpr size_t ALLOC_BLOCK = 8192;

        TVector<ui64> quantizedDataStorage;

        ui32 nonDefaultValuesCount = 0;

        auto onSrcNonDefaultValueCallback = [&, valueQuantizer, defaultQuantizedBin] (ui32 idx, TSrc value) {
            auto quantizedBin = valueQuantizer.Quantize(value);
            if (quantizedBin != defaultQuantizedBin) {
                indexingBuilder->AddOrdered(idx);

                if (nonDefaultValuesCount % (ALLOC_BLOCK * sizeof(ui64) / sizeof(TStoredDstValue)) == 0) {
                    quantizedDataStorage.yresize(nonDefaultValuesCount + ALLOC_BLOCK);
                }
                ((TStoredDstValue*)quantizedDataStorage.data())[nonDefaultValuesCount] = quantizedBin;
                ++nonDefaultValuesCount;
            }
        };

        if (const auto* denseSrcFeature = dynamic_cast<const TDenseSrcData*>(&srcFeature)){
            denseSrcFeature->GetData()->ForEach(onSrcNonDefaultValueCallback);
        } else if (const auto* sparseSrcFeature = dynamic_cast<const TSparseSrcData*>(&srcFeature)) {
            const auto& sparseArray = sparseSrcFeature->GetData();
            sparseArray.ForEachNonDefault(onSrcNonDefaultValueCallback);
        } else {
            CB_ENSURE_INTERNAL(false, "MakeQuantizedColumnWithDefaultBin: unsupported src feature type");
        }

        *dstFeature = MakeHolder<TSparseCompressedValuesHolderImpl<TDst, DstFeatureValuesType>>(
            srcFeature.GetId(),
            TSparseCompressedArray<TDst, ui32>(
                indexingBuilder->Build(srcFeature.GetSize()),
                TCompressedArray(
                    nonDefaultValuesCount,
                    valueQuantizer.GetDstBitsPerKey(),
                    std::move(quantizedDataStorage)
                ),
                std::move(defaultQuantizedBin)
            )
        );
    }

    // TCallback accepts (dstIndex, quantizedValue) arguments
    template <class TSrc, EFeatureValuesType SrcFeatureValuesType, class TCallback>
    static void QuantizeNonDefaultValues(
        const TTypedFeatureValuesHolder<TSrc, SrcFeatureValuesType>& srcFeature,
        const TIncrementalDenseIndexing& incrementalDenseIndexing,
        TValueQuantizer<TSrc, SrcFeatureValuesType> valueQuantizer,
        NPar::TLocalExecutor* localExecutor,
        TCallback&& callback
    ) {
        using TDenseSrcData = TPolymorphicArrayValuesHolder<TSrc, SrcFeatureValuesType>;
        using TSparseSrcData = TSparsePolymorphicArrayValuesHolder<TSrc, SrcFeatureValuesType>;

        if (const auto* denseSrcFeature = dynamic_cast<const TDenseSrcData*>(&srcFeature)){
            if (const auto* nontrivialIncrementalIndexing
                = GetIf<TIndexedSubset<ui32>>(&incrementalDenseIndexing.SrcSubsetIndexing))
            {
                TConstArrayRef<ui32> dstIndices
                    = Get<TIndexedSubset<ui32>>(incrementalDenseIndexing.DstIndexing);

                denseSrcFeature->GetData()->CloneWithNewSubsetIndexing(
                    &incrementalDenseIndexing.SrcSubsetIndexing
                )->ParallelForEach(
                    [=] (ui32 i, TSrc srcValue) {
                        callback(dstIndices[i], valueQuantizer.Quantize(srcValue));
                    },
                    localExecutor
                );
            } else {
                denseSrcFeature->GetData()->ParallelForEach(
                    [=] (ui32 dstIdx, TSrc srcValue) {
                        callback(dstIdx, valueQuantizer.Quantize(srcValue));
                    },
                    localExecutor
                );
            }
        } else if (const auto* sparseSrcFeature = dynamic_cast<const TSparseSrcData*>(&srcFeature)) {
            const auto& sparseArray = sparseSrcFeature->GetData();
            sparseArray.ForEachNonDefault(
                [=] (ui32 dstIdx, TSrc srcValue) {
                    callback(dstIdx, valueQuantizer.Quantize(srcValue));
                }
            );
        } else {
            CB_ENSURE_INTERNAL(false, "QuantizeNonDefaultValues: unsupported src feature type");
        }
    }

    template <
        class TStoredDstValue,
        class TDst,
        EFeatureValuesType DstFeatureValuesType,
        class TSrc,
        EFeatureValuesType SrcFeatureValuesType>
    static void MakeQuantizedColumnWithoutDefaultBin(
        const TTypedFeatureValuesHolder<TSrc, SrcFeatureValuesType>& srcFeature,
        const TIncrementalDenseIndexing& incrementalDenseIndexing,
        TValueQuantizer<TSrc, SrcFeatureValuesType> valueQuantizer,
        const TFeaturesArraySubsetIndexing* dstSubsetIndexing,
        NPar::TLocalExecutor* localExecutor,

        // pass as parameter to enable type deduction
        THolder<TTypedFeatureValuesHolder<TDst, DstFeatureValuesType>>* dstFeature
    ) {
        using TSparseSrcData = TSparsePolymorphicArrayValuesHolder<TSrc, SrcFeatureValuesType>;

        const ui32 dstBitsPerKey = valueQuantizer.GetDstBitsPerKey();

        Y_ASSERT(dstBitsPerKey == sizeof(TStoredDstValue) * CHAR_BIT);

        TCompressedArray dstStorage
            = TCompressedArray::CreateWithUninitializedData(srcFeature.GetSize(), dstBitsPerKey);

        TArrayRef<TStoredDstValue> dstArrayRef = dstStorage.GetRawArray<TStoredDstValue>();

        if (const auto* sparseSrcFeature = dynamic_cast<const TSparseSrcData*>(&srcFeature)) {
            const auto& sparseArray = sparseSrcFeature->GetData();
            if (sparseArray.GetDefaultSize()) {
                /* this is for consistency with dense data -
                    is case of cat features default value is not added to CatFeaturesPerfectHash
                    if it is not present in source data
                */
                const TStoredDstValue quantizedSrcDefaultValue
                    = valueQuantizer.Quantize(sparseArray.GetDefaultValue());

                ParallelFill(quantizedSrcDefaultValue, /*blockSize*/ Nothing(), localExecutor, dstArrayRef);
            }
        }

        QuantizeNonDefaultValues(
            srcFeature,
            incrementalDenseIndexing,
            std::move(valueQuantizer),
            localExecutor,
            [=] (ui32 dstIdx, ui32 quantizedValue) {
                dstArrayRef[dstIdx] = (TStoredDstValue)quantizedValue;
            }
        );

        *dstFeature = MakeHolder<TCompressedValuesHolderImpl<TDst, DstFeatureValuesType>>(
            srcFeature.GetId(),
            std::move(dstStorage),
            dstSubsetIndexing
        );
    }

    template <
        class TDst,
        EFeatureValuesType DstFeatureValuesType,
        class TSrc,
        EFeatureValuesType SrcFeatureValuesType>
    static void MakeQuantizedColumn(
        const TTypedFeatureValuesHolder<TSrc, SrcFeatureValuesType>& srcFeature,
        const TQuantizedFeaturesInfo& quantizedFeaturesInfo,
        const TIncrementalDenseIndexing& incrementalDenseIndexing,
        ESparseArrayIndexingType sparseArrayIndexingType,
        const TFeaturesArraySubsetIndexing* dstSubsetIndexing,
        NPar::TLocalExecutor* localExecutor,

        // pass as parameter to enable type deduction
        THolder<TTypedFeatureValuesHolder<TDst, DstFeatureValuesType>>* dstFeature
    ) {
        TValueQuantizer<TSrc, SrcFeatureValuesType> valueQuantizer(quantizedFeaturesInfo, srcFeature.GetId());

        // dispatch by storedDstValueType
        auto makeQuantizedColumnForStoredDstValue = [&] (auto storedDstValueExample) {
            if (valueQuantizer.GetDefaultBin()) {
                MakeQuantizedColumnWithDefaultBin<decltype(storedDstValueExample)>(
                    srcFeature,
                    std::move(valueQuantizer),
                    sparseArrayIndexingType,
                    dstFeature
                );
            } else {
                MakeQuantizedColumnWithoutDefaultBin<decltype(storedDstValueExample)>(
                    srcFeature,
                    incrementalDenseIndexing,
                    std::move(valueQuantizer),
                    dstSubsetIndexing,
                    localExecutor,
                    dstFeature
                );
            }
        };

        switch (valueQuantizer.GetDstBitsPerKey()) {
            case 8:
                makeQuantizedColumnForStoredDstValue(ui8());
                break;
            case 16:
                makeQuantizedColumnForStoredDstValue(ui16());
                break;
            case 32:
                makeQuantizedColumnForStoredDstValue(ui32());
                break;
            default:
                CB_ENSURE_INTERNAL(false, "MakeQuantizedColumn: unsupported bits per key");
        }
    }

    TMaybe<ui32> GetDefaultQuantizedValue(
        const TQuantizedFeaturesInfo& quantizedFeaturesInfo,
        TFeatureIdxWithType featureWithType
    ) {
        switch (featureWithType.FeatureType) {
            case EFeatureType::Float:
                return TValueQuantizer<float, EFeatureValuesType::Float>(
                    quantizedFeaturesInfo,
                    TFloatFeatureIdx(featureWithType.FeatureIdx)
                ).GetDefaultBin();
            case EFeatureType::Categorical:
                return TValueQuantizer<ui32, EFeatureValuesType::HashedCategorical>(
                    quantizedFeaturesInfo,
                    TCatFeatureIdx(featureWithType.FeatureIdx)
                ).GetDefaultBin();
            default:
                CB_ENSURE(
                    false,
                    "GetDefaultQuantizedValue is not supported for features of type "
                    << featureWithType.FeatureType
                );
        }
        Y_UNREACHABLE();
    }


    class TColumnsQuantizer {
    public:
        TColumnsQuantizer(
            bool clearSrcObjectsData,
            const TQuantizationOptions& options,
            const TIncrementalDenseIndexing& incrementalDenseIndexing,
            const TFeaturesLayout& featuresLayout,
            const TFeaturesArraySubsetIndexing* quantizedDataSubsetIndexing,
            NPar::TLocalExecutor* localExecutor,
            TRawObjectsData* rawObjectsData,
            TQuantizedForCPUObjectsData* quantizedObjectsData
        )
            : ClearSrcObjectsData(clearSrcObjectsData)
            , Options(options)
            , IncrementalDenseIndexing(incrementalDenseIndexing)
            , FeaturesLayout(featuresLayout)
            , QuantizedDataSubsetIndexing(quantizedDataSubsetIndexing)
            , LocalExecutor(localExecutor)
            , RawObjectsData(rawObjectsData)
            , QuantizedObjectsData(quantizedObjectsData)
        {
            ui64 cpuRamUsage = NMemInfo::GetMemInfo().RSS;
            OutputWarningIfCpuRamUsageOverLimit(cpuRamUsage, options.CpuRamLimit);

            ResourceConstrainedExecutor.ConstructInPlace(
                "CPU RAM",
                options.CpuRamLimit - Min(cpuRamUsage, options.CpuRamLimit),
                /*lenientMode*/ true,
                localExecutor
            );
        }

    public:
        template <
            class TSrc,
            EFeatureValuesType SrcFeatureValuesType,
            class TDst,
            EFeatureValuesType DstFeatureValuesType>
        void QuantizeAndClearSrcData(
            THolder<TTypedFeatureValuesHolder<TSrc, SrcFeatureValuesType>>* srcColumn,
            THolder<TTypedFeatureValuesHolder<TDst, DstFeatureValuesType>>* dstColumn
        ) const {
            MakeQuantizedColumn(
                **srcColumn,
                *QuantizedObjectsData->Data.QuantizedFeaturesInfo,
                IncrementalDenseIndexing,
                Options.SparseArrayIndexingType,
                QuantizedDataSubsetIndexing,
                LocalExecutor,
                dstColumn
            );
            if (ClearSrcObjectsData) {
                srcColumn->Destroy();
            }
        }

        void QuantizeAndClearSrcData(TFloatFeatureIdx floatFeatureIdx) const {
            QuantizeAndClearSrcData(
                &(RawObjectsData->FloatFeatures[*floatFeatureIdx]),
                &(QuantizedObjectsData->Data.FloatFeatures[*floatFeatureIdx])
            );
        }

        void QuantizeAndClearSrcData(TCatFeatureIdx catFeatureIdx) const {
            QuantizeAndClearSrcData(
                &(RawObjectsData->CatFeatures[*catFeatureIdx]),
                &(QuantizedObjectsData->Data.CatFeatures[*catFeatureIdx])
            );
        }

        template <class T, EFeatureValuesType FeatureValuesType, class TCallback>
        void QuantizeNonDefaultValuesAndClearSrcData(
            THolder<TTypedFeatureValuesHolder<T, FeatureValuesType>>* srcColumn,
            TCallback&& callback
        ) const {
            NCB::QuantizeNonDefaultValues(
                **srcColumn,
                IncrementalDenseIndexing,
                TValueQuantizer<T, FeatureValuesType>(
                    *QuantizedObjectsData->Data.QuantizedFeaturesInfo,
                    (*srcColumn)->GetId()
                ),
                LocalExecutor,
                std::move(callback)
            );
            if (ClearSrcObjectsData) {
                srcColumn->Destroy();
            }
        }


        template <EFeatureValuesType FeatureValuesType>
        void AggregateFeatures(ui32 aggregateIdx) const;

        template <EFeatureValuesType FeatureValuesType>
        void ScheduleAggregateFeatures();


        bool IsInAggregatedColumn(ui32 flatFeatureIdx) const {
            if (QuantizedObjectsData
                    ->ExclusiveFeatureBundlesData.FlatFeatureIndexToBundlePart[flatFeatureIdx])
            {
                return true;
            }
            if (QuantizedObjectsData
                    ->PackedBinaryFeaturesData.FlatFeatureIndexToPackedBinaryIndex[flatFeatureIdx])
            {
                return true;
            }
            if (QuantizedObjectsData
                    ->FeaturesGroupsData.FlatFeatureIndexToGroupPart[flatFeatureIdx])
            {
                return true;
            }
            return false;
        }

        template <EFeatureType FeatureType, class TSrcValue, EFeatureValuesType SrcFeatureValuesType>
        void ScheduleNonAggregatedFeaturesForType() {
            const ui32 objectCount = QuantizedDataSubsetIndexing->Size();

            const TQuantizedFeaturesInfo& quantizedFeaturesInfo
                = *QuantizedObjectsData->Data.QuantizedFeaturesInfo;

            FeaturesLayout.IterateOverAvailableFeatures<FeatureType>(
                [&] (TFeatureIdx<FeatureType> perTypeFeatureIdx) {
                    const ui32 flatFeatureIdx
                        = FeaturesLayout.GetExternalFeatureIdx(*perTypeFeatureIdx, FeatureType);

                    if (IsInAggregatedColumn(flatFeatureIdx)) {
                        return;
                    }

                    TValueQuantizer<TSrcValue, SrcFeatureValuesType> valueQuantizer(
                        quantizedFeaturesInfo,
                        flatFeatureIdx
                    );

                    ResourceConstrainedExecutor->Add(
                        {
                            objectCount * (valueQuantizer.GetDstBitsPerKey() / CHAR_BIT),

                            [this, perTypeFeatureIdx] () {
                                QuantizeAndClearSrcData(perTypeFeatureIdx);
                            }
                        }
                    );
                }
            );
        }

        void ScheduleNonAggregatedFeatures() {
            ScheduleNonAggregatedFeaturesForType<EFeatureType::Float, float, EFeatureValuesType::Float>();
            ScheduleNonAggregatedFeaturesForType<
                EFeatureType::Categorical,
                ui32, EFeatureValuesType::HashedCategorical>();
        };

        void Do();

    public: // public to make it easier to access from TColumnsAggregator
        bool ClearSrcObjectsData;
        const TQuantizationOptions& Options;
        const TIncrementalDenseIndexing& IncrementalDenseIndexing;
        const TFeaturesLayout& FeaturesLayout;
        const TFeaturesArraySubsetIndexing* QuantizedDataSubsetIndexing;
        NPar::TLocalExecutor* LocalExecutor;
        TRawObjectsData* RawObjectsData;
        TQuantizedForCPUObjectsData* QuantizedObjectsData;

        // TMaybe because of delayed initialization
        TMaybe<TResourceConstrainedExecutor> ResourceConstrainedExecutor;
    };


    template <EFeatureValuesType FeatureValuesType>
    class TColumnsAggregator {
    public:
        /* TAggregationContext is some small copyable information for (aggregateIdx, partIdx) to use in
         *  AddToAggregate.
         *  Redefined in implementations.
         */
        using TAggregationContext = TNothing;

    public:
        ui32 GetAggregateCount() const;

        ui32 GetAggregatePartsCount(ui32 aggregateIdx) const;

        ui32 GetAggregateBitsPerKey(ui32 aggregateIdx) const;

        TFeatureIdxWithType GetSrcPart(ui32 aggregateIdx, ui32 partIdx) const;

        ui32 GetDefaultValue(ui32 aggregateIdx) const;

        TAggregationContext GetAggregationContext(ui32 aggregateIdx, ui32 partIdx) const;

        template <class TDstValue>
        Y_FORCE_INLINE static void AddToAggregate(
            TAggregationContext aggregationContext,
            ui32 quantizedSrcValue,
            TDstValue *dstValue);

        void SaveData(ui32 aggregateIdx, TCompressedArray&& aggregatedData);
    };

    template <>
    class TColumnsAggregator<EFeatureValuesType::ExclusiveFeatureBundle> {
        using TAggregationContext = ui32; // boundsBegin

    public:
        TColumnsAggregator(const TColumnsQuantizer& columnsQuantizer)
            : ColumnsQuantizer(columnsQuantizer)
            , MetaData(columnsQuantizer.QuantizedObjectsData->ExclusiveFeatureBundlesData.MetaData)
        {}

        ui32 GetAggregateCount() const {
            return (ui32)MetaData.size();
        }

        ui32 GetAggregatePartsCount(ui32 aggregateIdx) const {
            return (ui32)MetaData[aggregateIdx].Parts.size();
        }

        ui32 GetAggregateBitsPerKey(ui32 aggregateIdx) const {
            return (ui32)MetaData[aggregateIdx].SizeInBytes * CHAR_BIT;
        }

        TFeatureIdxWithType GetSrcPart(ui32 aggregateIdx, ui32 partIdx) const {
            return MetaData[aggregateIdx].Parts[partIdx];
        }

        ui32 GetDefaultValue(ui32 aggregateIdx) const {
            return MetaData[aggregateIdx].Parts.back().Bounds.End;
        }

        TAggregationContext GetAggregationContext(ui32 aggregateIdx, ui32 partIdx) const {
            return (ui32)MetaData[aggregateIdx].Parts[partIdx].Bounds.Begin;
        }

        template <class TDstValue>
        Y_FORCE_INLINE static void AddToAggregate(
            TAggregationContext boundsBegin,
            ui32 quantizedSrcValue,
            TDstValue *dstValue
        ) {
            if (quantizedSrcValue) {
                *dstValue = (TDstValue)(boundsBegin + quantizedSrcValue - 1);
            }
        }

        void SaveData(ui32 aggregateIdx, TCompressedArray&& aggregatedData) {
            auto& bundleData
                = ColumnsQuantizer.QuantizedObjectsData->ExclusiveFeatureBundlesData.SrcData[aggregateIdx];

            bundleData = MakeHolder<TExclusiveFeatureBundleArrayHolder>(
                0, // unused
                std::move(aggregatedData),
                ColumnsQuantizer.QuantizedDataSubsetIndexing
            );

            auto& quantizedData = ColumnsQuantizer.QuantizedObjectsData->Data;

            for (const auto& part : MetaData[aggregateIdx].Parts) {
                const ui32 flatFeatureIdx = ColumnsQuantizer.FeaturesLayout.GetExternalFeatureIdx(
                    part.FeatureIdx,
                    part.FeatureType
                );

                switch (part.FeatureType) {
                    case EFeatureType::Float:
                        quantizedData.FloatFeatures[part.FeatureIdx].Reset(
                            new TQuantizedFloatBundlePartValuesHolder(
                                flatFeatureIdx,
                                bundleData.Get(),
                                part.Bounds
                            )
                        );
                        break;
                    case EFeatureType::Categorical:
                        quantizedData.CatFeatures[part.FeatureIdx].Reset(
                            new TQuantizedCatBundlePartValuesHolder(
                                flatFeatureIdx,
                                bundleData.Get(),
                                part.Bounds
                            )
                        );
                        break;
                    default:
                        Y_FAIL(); // has already been checked above
                }
            }
        }

    private:
        const TColumnsQuantizer& ColumnsQuantizer;
        TConstArrayRef<TExclusiveFeaturesBundle> MetaData;
    };


    template <>
    class TColumnsAggregator<EFeatureValuesType::BinaryPack> {
        struct TAggregationContext {
            ui8 BitIdx;
            ui32 Mask;
        };

        constexpr static size_t BITS_PER_PACK = sizeof(TBinaryFeaturesPack) * CHAR_BIT;

    public:
        TColumnsAggregator(const TColumnsQuantizer& columnsQuantizer)
            : ColumnsQuantizer(columnsQuantizer)
            , PackedBinaryFeaturesData(columnsQuantizer.QuantizedObjectsData->PackedBinaryFeaturesData)
            , PackedBinaryToSrcIndex(PackedBinaryFeaturesData.PackedBinaryToSrcIndex)
        {}

        ui32 GetAggregateCount() const {
            return (ui32)PackedBinaryFeaturesData.SrcData.size();
        }

        ui32 GetAggregatePartsCount(ui32 aggregateIdx) const {
            const size_t startIdx = BITS_PER_PACK * aggregateIdx;
            return (ui32)Min(BITS_PER_PACK, PackedBinaryToSrcIndex.size() - startIdx);
        }

        ui32 GetAggregateBitsPerKey(ui32 aggregateIdx) const {
            Y_UNUSED(aggregateIdx);
            return (ui32) BITS_PER_PACK;
        }

        TFeatureIdxWithType GetSrcPart(ui32 aggregateIdx, ui32 partIdx) const {
            return PackedBinaryToSrcIndex[BITS_PER_PACK * aggregateIdx + partIdx];
        }

        ui32 GetDefaultValue(ui32 aggregateIdx) const {
            ui32 result = 0;

            for (auto bitIdx : xrange(GetAggregatePartsCount(aggregateIdx))) {
                TMaybe<ui32> defaultBin
                    = GetDefaultQuantizedValue(
                        *ColumnsQuantizer.QuantizedObjectsData->Data.QuantizedFeaturesInfo,
                        GetSrcPart(aggregateIdx, bitIdx)
                    );
                if (defaultBin) {
                    Y_ASSERT(*defaultBin <= 1);
                    result |= *defaultBin << bitIdx;
                }
            }

            return result;
        }

        TAggregationContext GetAggregationContext(ui32 aggregateIdx, ui32 partIdx) const {
            Y_UNUSED(aggregateIdx);
            return TAggregationContext{(ui8)partIdx, ~(ui32(1) << partIdx)};
        }

        template <class TDstValue>
        Y_FORCE_INLINE static void AddToAggregate(
            TAggregationContext aggregationContext,
            ui32 quantizedSrcValue,
            TDstValue *dstValue
        ) {
            Y_ASSERT(quantizedSrcValue <= 1);
            *dstValue = (TDstValue)((ui32(*dstValue) & aggregationContext.Mask)
                | (quantizedSrcValue << aggregationContext.BitIdx));
        }

        void SaveData(ui32 aggregateIdx, TCompressedArray&& aggregatedData) {
            auto& packedData = PackedBinaryFeaturesData.SrcData[aggregateIdx];

            packedData = MakeHolder<TBinaryPacksArrayHolder>(
                0, // unused
                std::move(aggregatedData),
                ColumnsQuantizer.QuantizedDataSubsetIndexing
            );

            auto& quantizedData = ColumnsQuantizer.QuantizedObjectsData->Data;

            for (auto bitIdx : xrange(GetAggregatePartsCount(aggregateIdx))) {
                const auto part = GetSrcPart(aggregateIdx, bitIdx);

                const ui32 flatFeatureIdx = ColumnsQuantizer.FeaturesLayout.GetExternalFeatureIdx(
                    part.FeatureIdx,
                    part.FeatureType
                );

                switch (part.FeatureType) {
                    case EFeatureType::Float:
                        quantizedData.FloatFeatures[part.FeatureIdx].Reset(
                            new TQuantizedFloatPackedBinaryValuesHolder(
                                flatFeatureIdx,
                                packedData.Get(),
                                bitIdx
                            )
                        );
                        break;
                    case EFeatureType::Categorical:
                        quantizedData.CatFeatures[part.FeatureIdx].Reset(
                            new TQuantizedCatPackedBinaryValuesHolder(
                                flatFeatureIdx,
                                packedData.Get(),
                                bitIdx
                            )
                        );
                        break;
                    default:
                        Y_FAIL(); // has already been checked above
                }
            }
        }

    private:
        const TColumnsQuantizer& ColumnsQuantizer;
        TPackedBinaryFeaturesData& PackedBinaryFeaturesData;
        TConstArrayRef<TFeatureIdxWithType> PackedBinaryToSrcIndex;
    };


    template <>
    class TColumnsAggregator<EFeatureValuesType::FeaturesGroup> {
        using TAggregationContext = ui32; // partShift

    public:
        TColumnsAggregator(const TColumnsQuantizer& columnsQuantizer)
            : ColumnsQuantizer(columnsQuantizer)
            , MetaData(columnsQuantizer.QuantizedObjectsData->FeaturesGroupsData.MetaData)
        {}

        ui32 GetAggregateCount() const {
            return (ui32)MetaData.size();
        }

        ui32 GetAggregatePartsCount(ui32 aggregateIdx) const {
            return (ui32)MetaData[aggregateIdx].Parts.size();
        }

        ui32 GetAggregateBitsPerKey(ui32 aggregateIdx) const {
            return (ui32)MetaData[aggregateIdx].GetSizeInBytes() * CHAR_BIT;
        }

        TFeatureIdxWithType GetSrcPart(ui32 aggregateIdx, ui32 partIdx) const {
            return MetaData[aggregateIdx].Parts[partIdx];
        }

        ui32 GetDefaultValue(ui32 aggregateIdx) const {
            ui32 result = 0;

            for (auto partIdx : xrange(GetAggregatePartsCount(aggregateIdx))) {
                TMaybe<ui32> defaultBin
                    = GetDefaultQuantizedValue(
                        *ColumnsQuantizer.QuantizedObjectsData->Data.QuantizedFeaturesInfo,
                        GetSrcPart(aggregateIdx, partIdx)
                    );
                if (defaultBin) {
                    result |= *defaultBin << (partIdx * CHAR_BIT);
                }
            }

            return result;
        }

        TAggregationContext GetAggregationContext(ui32 aggregateIdx, ui32 partIdx) const {
            Y_UNUSED(aggregateIdx);
            return (ui32)partIdx * CHAR_BIT;
        }

        template <class TDstValue>
        Y_FORCE_INLINE static void AddToAggregate(
            TAggregationContext partShift,
            ui32 quantizedSrcValue,
            TDstValue *dstValue
        ) {
            *dstValue |= quantizedSrcValue << partShift;
        }

        void SaveData(ui32 aggregateIdx, TCompressedArray&& aggregatedData) {
            auto& groupData = ColumnsQuantizer.QuantizedObjectsData->FeaturesGroupsData.SrcData[aggregateIdx];

            groupData = MakeHolder<TFeaturesGroupArrayHolder>(
                0, // unused
                std::move(aggregatedData),
                ColumnsQuantizer.QuantizedDataSubsetIndexing
            );

            auto& quantizedData = ColumnsQuantizer.QuantizedObjectsData->Data;

            for (auto partIdx : xrange(GetAggregatePartsCount(aggregateIdx))) {
                const auto& part = MetaData[aggregateIdx].Parts[partIdx];

                const ui32 flatFeatureIdx = ColumnsQuantizer.FeaturesLayout.GetExternalFeatureIdx(
                    part.FeatureIdx,
                    part.FeatureType
                );

                switch (part.FeatureType) {
                    case EFeatureType::Float:
                        quantizedData.FloatFeatures[part.FeatureIdx].Reset(
                            new TQuantizedFloatGroupPartValuesHolder(flatFeatureIdx, groupData.Get(), partIdx)
                        );
                        break;
                    default:
                        Y_FAIL(); // has already been checked above
                }
            }
        }

    private:
        const TColumnsQuantizer& ColumnsQuantizer;
        TConstArrayRef<TFeaturesGroup> MetaData;
    };


    template <EFeatureValuesType FeatureValuesType>
    void TColumnsQuantizer::AggregateFeatures(ui32 aggregateIdx) const {
        TColumnsAggregator<FeatureValuesType> columnsAggregator(*this);

        TCompressedArray dstStorage;

        // dispatch by storedType
        auto aggregateFeaturesWithStoredValueType = [&] (auto aggregateValueExample) {
            using TAggregateValue = decltype(aggregateValueExample);

            const ui32 bitsPerKey = sizeof(TAggregateValue) * CHAR_BIT;
            const ui32 objectCount = QuantizedDataSubsetIndexing->Size();

            dstStorage = TCompressedArray::CreateWithUninitializedData(objectCount, bitsPerKey);

            TArrayRef<TAggregateValue> dstDataRef = dstStorage.GetRawArray<TAggregateValue>();

            const TAggregateValue defaultValue
                = (TAggregateValue)columnsAggregator.GetDefaultValue(aggregateIdx);

            ParallelFill(defaultValue, /*blockSize*/ Nothing(), LocalExecutor, dstDataRef);

            for (auto partIdx : xrange(columnsAggregator.GetAggregatePartsCount(aggregateIdx))) {
                const auto aggregationContext = columnsAggregator.GetAggregationContext(aggregateIdx, partIdx);

                const TFeatureIdxWithType part = columnsAggregator.GetSrcPart(aggregateIdx, partIdx);

                switch (part.FeatureType) {
                    case EFeatureType::Float:
                        QuantizeNonDefaultValuesAndClearSrcData(
                            &(RawObjectsData->FloatFeatures[part.FeatureIdx]),
                            [=] (ui32 dstIdx, ui32 quantizedValue) {
                                TColumnsAggregator<FeatureValuesType>::AddToAggregate(
                                    aggregationContext,
                                    quantizedValue,
                                    &dstDataRef[dstIdx]
                                );
                            }
                        );
                        break;
                    case EFeatureType::Categorical:
                        QuantizeNonDefaultValuesAndClearSrcData(
                            &(RawObjectsData->CatFeatures[part.FeatureIdx]),
                            [=] (ui32 dstIdx, ui32 quantizedValue) {
                                TColumnsAggregator<FeatureValuesType>::AddToAggregate(
                                    aggregationContext,
                                    quantizedValue,
                                    &dstDataRef[dstIdx]
                                );
                            }
                        );
                        break;
                    default:
                        CB_ENSURE(
                            false,
                            "Feature bundling is not supported for features of type " << part.FeatureType
                        );
                }
            }
        };

        const ui32 bitsPerKey = columnsAggregator.GetAggregateBitsPerKey(aggregateIdx);

        switch (bitsPerKey) {
            case 8:
                aggregateFeaturesWithStoredValueType(ui8());
                break;
            case 16:
                aggregateFeaturesWithStoredValueType(ui16());
                break;
            case 32:
                aggregateFeaturesWithStoredValueType(ui32());
                break;
            default:
                CB_ENSURE_INTERNAL(false, "AggregateFeatures: unsupported bitsPerKey = " << bitsPerKey);
        }

        columnsAggregator.SaveData(aggregateIdx, std::move(dstStorage));
    }

    template <EFeatureValuesType FeatureValuesType>
    void TColumnsQuantizer::ScheduleAggregateFeatures() {
        const ui32 objectCount = QuantizedDataSubsetIndexing->Size();

        TColumnsAggregator<FeatureValuesType> columnsAggregator(*this);

        for (auto aggregateIdx : xrange(columnsAggregator.GetAggregateCount())) {
            ResourceConstrainedExecutor->Add(
                {
                    objectCount * (columnsAggregator.GetAggregateBitsPerKey(aggregateIdx) / CHAR_BIT),

                    [this, aggregateIdx] () {
                        AggregateFeatures<FeatureValuesType>(aggregateIdx);
                    }
                }
            );
        }
    }

    void TColumnsQuantizer::Do() {
        if (Options.CpuCompatibleFormat && Options.BundleExclusiveFeaturesForCpu) {
            ScheduleAggregateFeatures<EFeatureValuesType::ExclusiveFeatureBundle>();

            /*
             * call it only if bundleExclusiveFeatures because otherwise they've already been
             * created during Process(Float|Cat)Feature calls above
             */
            ScheduleNonAggregatedFeatures();
        }

        if (Options.CpuCompatibleFormat && Options.PackBinaryFeaturesForCpu) {
            ScheduleAggregateFeatures<EFeatureValuesType::BinaryPack>();
        }

        if (Options.CpuCompatibleFormat && Options.GroupFeaturesForCpu) {
            ScheduleAggregateFeatures<EFeatureValuesType::FeaturesGroup>();
        }

        ResourceConstrainedExecutor->ExecTasks();
    }


    static void ProcessFloatFeature(
        TFloatFeatureIdx floatFeatureIdx,
        const TFloatValuesHolder& srcFeature,
        const TSubsetIndexingForBuildBorders& subsetIndexingForBuildBorders,
        const TQuantizationOptions& options,
        const TInitialBorders& initialBorders,
        bool calcQuantizationAndNanModeOnly,
        bool storeFeaturesDataAsExternalValuesHolder,

        // can be TNothing if generateBordersOnly
        const TMaybe<TIncrementalDenseIndexing>& incrementalDenseIndexing,
        const TFeaturesArraySubsetIndexing* dstSubsetIndexing,  // can be nullptr if generateBordersOnly
        NPar::TLocalExecutor* localExecutor,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
        THolder<IQuantizedFloatValuesHolder>* dstQuantizedFeature // can be nullptr if generateBordersOnly
    ) {
        bool calculateNanMode = true;
        ENanMode nanMode = ENanMode::Forbidden;

        bool calculateQuantization = true;
        const NSplitSelection::TQuantization* quantization;
        NSplitSelection::TQuantization calculatedQuantization;

        {
            TReadGuard readGuard(quantizedFeaturesInfo->GetRWMutex());
            if (quantizedFeaturesInfo->HasNanMode(floatFeatureIdx)) {
                calculateNanMode = false;
                nanMode = quantizedFeaturesInfo->GetNanMode(floatFeatureIdx);
            }
            if (quantizedFeaturesInfo->HasQuantization(floatFeatureIdx)) {
                calculateQuantization = false;
                quantization = &(quantizedFeaturesInfo->GetQuantization(floatFeatureIdx));
            }
        }

        CB_ENSURE_INTERNAL(
            calculateNanMode == calculateQuantization,
            "Feature #" << srcFeature.GetId()
            << ": NanMode and quantization must be specified or not specified together"
        );

        if (calculateNanMode || calculateQuantization) {
            TMaybe<TVector<float>> initialBordersForFeature = Nothing();
            if (initialBorders) {
                initialBordersForFeature.ConstructInPlace(TVector<float>((*initialBorders)[floatFeatureIdx.Idx].begin(), (*initialBorders)[floatFeatureIdx.Idx].end()));
            }
            CalcQuantizationAndNanMode(
                srcFeature,
                subsetIndexingForBuildBorders,
                *quantizedFeaturesInfo,
                initialBordersForFeature,
                options.DefaultValueFractionToEnableSparseStorage,
                &nanMode,
                &calculatedQuantization
            );

            quantization = &calculatedQuantization;
        }

        // save, because calculatedQuantization can be moved to quantizedFeaturesInfo
        const size_t borderCount = quantization->Borders.size();

        if (calculateNanMode || calculateQuantization) {
            TWriteGuard writeGuard(quantizedFeaturesInfo->GetRWMutex());

            if (calculateNanMode) {
                quantizedFeaturesInfo->SetNanMode(floatFeatureIdx, nanMode);
            }
            if (calculateQuantization) {
                if (calculatedQuantization.Borders.empty()) {
                    CATBOOST_DEBUG_LOG << "Float Feature #" << srcFeature.GetId() << " is empty" << Endl;

                    quantizedFeaturesInfo->GetFeaturesLayout()->IgnoreExternalFeature(srcFeature.GetId());
                }

                quantizedFeaturesInfo->SetQuantization(floatFeatureIdx, std::move(calculatedQuantization));
            }
        }

        if (!calcQuantizationAndNanModeOnly && (borderCount != 0)) {
            if (storeFeaturesDataAsExternalValuesHolder) {
                // use GPU-only external columns
                *dstQuantizedFeature =
                    MakeExternalValuesHolder<
                        IQuantizedFloatValuesHolder,
                        TExternalFloatValuesHolder,
                        TExternalFloatSparseValuesHolder>(srcFeature, quantizedFeaturesInfo);
            } else if (!options.CpuCompatibleFormat ||
                !options.PackBinaryFeaturesForCpu ||
                (borderCount > 1)) // binary features are binarized later by packs
            {
                MakeQuantizedColumn(
                    srcFeature,
                    *quantizedFeaturesInfo,
                    *incrementalDenseIndexing,
                    options.SparseArrayIndexingType,
                    dstSubsetIndexing,
                    localExecutor,
                    dstQuantizedFeature
                );
            }
        }
    }


    static ui64 EstimateMemUsageForCatFeature(
        const THashedCatValuesHolder& srcFeature,
        bool storeFeaturesDataAsExternalValuesHolder
    ) {
        ui64 result = 0;

        const ui32 nonDefaultObjectCount = GetNonDefaultValuesCount(srcFeature);

        constexpr ui32 ESTIMATED_FEATURES_PERFECT_HASH_MAP_NODE_SIZE = 32;

        // assuming worst-case that all values will be added to Features Perfect Hash as new.
        result += ESTIMATED_FEATURES_PERFECT_HASH_MAP_NODE_SIZE * nonDefaultObjectCount;

        if (!storeFeaturesDataAsExternalValuesHolder) {
            // for storing quantized data
            // TODO(akhropov): support other bitsPerKey. MLTOOLS-2425
            result += sizeof(ui32) * nonDefaultObjectCount;
        }

        return result;
    }


    static void ProcessCatFeature(
        TCatFeatureIdx catFeatureIdx,
        const THashedCatValuesHolder& srcFeature,
        const TQuantizationOptions& options,
        bool bundleExclusiveFeatures,
        bool storeFeaturesDataAsExternalValuesHolder,
        const TIncrementalDenseIndexing& incrementalDenseIndexing,
        const TFeaturesArraySubsetIndexing* dstSubsetIndexing,
        NPar::TLocalExecutor* localExecutor,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
        THolder<IQuantizedCatValuesHolder>* dstQuantizedFeature
    ) {
        const bool updatePerfectHashOnly = bundleExclusiveFeatures;

        // GPU-only external columns
        const bool quantizeData = !updatePerfectHashOnly && !storeFeaturesDataAsExternalValuesHolder;


        TCompressedArray quantizedDataStorage;

        auto onNonDefaultValues = [&] (
            const ITypedArraySubset<ui32>& srcNonDefaultValues,
            TMaybe<TDefaultValue<ui32>> srcDefaultValue) {

            // can quantize data at first pass only if data is dense and default bin value won't be determined
            const bool quantizeDataAtFirstPass
                = quantizeData && !srcDefaultValue.Defined() &&
                    !options.DefaultValueFractionToEnableSparseStorage.Defined();

            TArrayRef<ui32> quantizedDataValue;

            if (quantizeDataAtFirstPass) {
                // TODO(akhropov): support other bitsPerKey. MLTOOLS-2425
                const ui32 bitsPerKey = 32;

                quantizedDataStorage
                    = TCompressedArray::CreateWithUninitializedData(srcNonDefaultValues.GetSize(), bitsPerKey);
                quantizedDataValue = quantizedDataStorage.GetRawArray<ui32>();
            }

            TCatFeaturesPerfectHashHelper catFeaturesPerfectHashHelper(quantizedFeaturesInfo);

            catFeaturesPerfectHashHelper.UpdatePerfectHashAndMaybeQuantize(
                catFeatureIdx,
                srcNonDefaultValues,
                /*mapMostFrequentValueTo0*/ bundleExclusiveFeatures,
                srcDefaultValue,
                options.DefaultValueFractionToEnableSparseStorage,
                quantizeDataAtFirstPass ? TMaybe<TArrayRef<ui32>*>(&quantizedDataValue) : Nothing()
            );
        };

        if (const auto* denseSrcFeature = dynamic_cast<const THashedCatArrayValuesHolder*>(&srcFeature)) {
            onNonDefaultValues(*denseSrcFeature->GetData(), Nothing());
        } else if (const auto* sparseSrcFeature
                       = dynamic_cast<const THashedCatSparseValuesHolder*>(&srcFeature))
        {
            const TConstPolymorphicValuesSparseArray<ui32, ui32>& sparseArray = sparseSrcFeature->GetData();

            TFeaturesArraySubsetIndexing nonDefaultIndexing(
                TFullSubset<ui32>(sparseArray.GetNonDefaultSize())
            );

            TMaybe<TDefaultValue<ui32>> defaultValue;
            if (sparseArray.GetDefaultSize()) {
                defaultValue.ConstructInPlace(sparseArray.GetDefaultValue(), sparseArray.GetDefaultSize());
            }

            onNonDefaultValues(
                *sparseArray.GetNonDefaultValues().GetImpl().GetSubset(&nonDefaultIndexing),
                defaultValue
            );
        } else {
            CB_ENSURE_INTERNAL(false, "ProcessCatFeature: unsupported src feature type");
        }

        auto uniqueValuesCounts = quantizedFeaturesInfo->GetUniqueValuesCounts(catFeatureIdx);
        if (uniqueValuesCounts.OnLearnOnly > 1) {
            if (!updatePerfectHashOnly) {
                if (storeFeaturesDataAsExternalValuesHolder) {
                    *dstQuantizedFeature =
                        MakeExternalValuesHolder<
                            IQuantizedCatValuesHolder,
                            TExternalCatValuesHolder,
                            TExternalCatSparseValuesHolder>(srcFeature, quantizedFeaturesInfo);
                } else if (quantizedDataStorage.GetSize()) {
                    // was initialized at first pass
                    *dstQuantizedFeature = MakeHolder<TQuantizedCatValuesHolder>(
                        srcFeature.GetId(),
                        std::move(quantizedDataStorage),
                        dstSubsetIndexing
                    );
                } else {
                    MakeQuantizedColumn(
                        srcFeature,
                        *quantizedFeaturesInfo,
                        incrementalDenseIndexing,
                        options.SparseArrayIndexingType,
                        dstSubsetIndexing,
                        localExecutor,
                        dstQuantizedFeature
                    );
                }
            }
        } else {
            CATBOOST_DEBUG_LOG << "Categorical Feature #" << srcFeature.GetId() << " is constant" << Endl;

            quantizedFeaturesInfo->GetFeaturesLayout()->IgnoreExternalFeature(srcFeature.GetId());
        }
    }


    static void CreateDictionaries(
        TConstArrayRef<THolder<TStringTextValuesHolder>> textFeatures,
        const TFeaturesLayout& featuresLayout,
        const NCatboostOptions::TRuntimeTextOptions& textOptions,
        TTextDigitizers* textDigitizers
    ) {
        for (ui32 tokenizedFeatureIdx: xrange(textOptions.TokenizedFeatureCount())) {
            const auto& featureDescription = textOptions.GetTokenizedFeatureDescription(tokenizedFeatureIdx);
            const ui32 textFeatureIdx = featureDescription.TextFeatureId;

            if (textDigitizers->HasDictionary(tokenizedFeatureIdx) ||
                !featuresLayout.GetInternalFeatureMetaInfo(textFeatureIdx, EFeatureType::Text).IsAvailable) {
                continue;
            }

            const auto& srcDenseFeature = dynamic_cast<const TStringTextArrayValuesHolder&>(
                *textFeatures[textFeatureIdx]
            );
            ITypedArraySubsetPtr<TString> textFeature = srcDenseFeature.GetData();

            auto dictionary = CreateDictionary(
                TIterableTextFeature(textFeature),
                textOptions.GetDictionaryOptions(featureDescription.DictionaryId.Get()),
                textDigitizers->GetTokenizer()
            );
            textDigitizers->AddDictionary(textFeatureIdx, tokenizedFeatureIdx, dictionary);
        }
    }

    static void AddTokenizedFeaturesToFeatureLayout(
        const NCatboostOptions::TRuntimeTextOptions& textOptions,
        TFeaturesLayout* featuresLayout
    ) {
        const auto& featureDescriptions = textOptions.GetTokenizedFeatureDescriptions();
        const ui32 tokenizedFeatureCount = featureDescriptions.size();

        TVector<TString> tokenizedFeatureNames;
        tokenizedFeatureNames.reserve(tokenizedFeatureCount);
        for (ui32 tokenizedFeatureIdx: xrange(tokenizedFeatureCount)) {
            tokenizedFeatureNames.push_back(featureDescriptions[tokenizedFeatureIdx].FeatureId);
        }

        TFeaturesLayout layoutWithTokenizedFeatures;

        ui32 tokenizedFeatureIdx = 0;
        for (ui32 featureIdx = 0; featureIdx < featuresLayout->GetExternalFeatureCount(); featureIdx++) {
            auto& metaInfo = featuresLayout->GetExternalFeatureMetaInfo(featureIdx);
            if (metaInfo.Type == EFeatureType::Text) {
                layoutWithTokenizedFeatures.AddFeature(
                    TFeatureMetaInfo{
                        EFeatureType::Text,
                        tokenizedFeatureNames[tokenizedFeatureIdx]
                    }
                );
                tokenizedFeatureIdx++;
            } else {
                layoutWithTokenizedFeatures.AddFeature(TFeatureMetaInfo(metaInfo));
            }
        }

        for (; tokenizedFeatureIdx < tokenizedFeatureCount; tokenizedFeatureIdx++) {
            layoutWithTokenizedFeatures.AddFeature(
                TFeatureMetaInfo{
                    EFeatureType::Text,
                    tokenizedFeatureNames[tokenizedFeatureIdx]
                }
            );
        }

        *featuresLayout = std::move(layoutWithTokenizedFeatures);
    }


    static void ProcessTextFeatures(
        TConstArrayRef<THolder<TStringTextValuesHolder>> textFeatures,
        const TFeaturesArraySubsetIndexing* dstSubsetIndexing,
        const TTextDigitizers& textDigitizers,
        TArrayRef<THolder<TTokenizedTextValuesHolder>> dstQuantizedFeatures,
        NPar::TLocalExecutor* localExecutor
    ) {
        textDigitizers.Apply(
            [textFeatures](ui32 textFeatureIdx) {
                const TStringTextValuesHolder& srcFeature = *textFeatures[textFeatureIdx];
                const auto& srcDenseFeature = dynamic_cast<const TStringTextArrayValuesHolder&>(srcFeature);
                return TIterableTextFeature<ITypedArraySubsetPtr<TString>>(srcDenseFeature.GetData());
            },
            [&](ui32 tokenizedFeatureIdx, TVector<TText>&& tokenizedFeature) {
                dstQuantizedFeatures[tokenizedFeatureIdx] = MakeHolder<TTokenizedTextArrayValuesHolder>(
                    tokenizedFeatureIdx,
                    TTextColumn::CreateOwning(std::move(tokenizedFeature)),
                    dstSubsetIndexing
                );
            },
            localExecutor
        );
    }


    static bool IsFloatFeatureToBeBinarized(
        const TQuantizationOptions& options,
        TQuantizedFeaturesInfo& quantizedFeaturesInfo, // non const because of GetRWMutex
        TFloatFeatureIdx floatFeatureIdx
    ) {
        if (!options.CpuCompatibleFormat || !options.PackBinaryFeaturesForCpu) {
            return false;
        }

        {
            TReadGuard guard(quantizedFeaturesInfo.GetRWMutex());

            if (quantizedFeaturesInfo.GetFeaturesLayout()->GetInternalFeatureMetaInfo(
                    *floatFeatureIdx,
                    EFeatureType::Float
                ).IsAvailable &&
                (quantizedFeaturesInfo.GetBorders(floatFeatureIdx).size() == 1))
            {
                return true;
            }
        }
        return false;
    }

    static bool IsCatFeatureToBeBinarized(
        const TQuantizationOptions& options,
        TQuantizedFeaturesInfo& quantizedFeaturesInfo, // non const because of GetRWMutex
        TCatFeatureIdx catFeatureIdx
    ) {
        if (!options.CpuCompatibleFormat || !options.PackBinaryFeaturesForCpu) {
            return false;
        }

        {
            TReadGuard guard(quantizedFeaturesInfo.GetRWMutex());

            if (quantizedFeaturesInfo.GetFeaturesLayout()->GetInternalFeatureMetaInfo(
                    *catFeatureIdx,
                    EFeatureType::Categorical
                ).IsAvailable &&
                (quantizedFeaturesInfo.GetUniqueValuesCounts(catFeatureIdx).OnAll == 2))
            {
                return true;
            }
        }
        return false;
    }

    static void AddIgnoredFeatures(const TFeaturesLayout& addFromLayout, TFeaturesLayout* updatedLayout) {
        auto featuresIntersectionSize = Min(
            addFromLayout.GetExternalFeatureCount(),
            updatedLayout->GetExternalFeatureCount()
        );

        for (auto i : xrange(featuresIntersectionSize)) {
            if (addFromLayout.GetExternalFeaturesMetaInfo()[i].IsIgnored) {
                updatedLayout->IgnoreExternalFeature(i);
            }
        }
    }


    static TFeaturesLayoutPtr InitFeaturesLayoutForQuantizedData(
        const TFeaturesLayout& rawObjectsDataLayout,
        const TFeaturesLayout& quantizedFeaturesInfoLayout
    ) {
        CheckCompatibleForQuantize(
            rawObjectsDataLayout,
            quantizedFeaturesInfoLayout,
            "data to quantize"
        );

        TFeaturesLayoutPtr featuresLayout = MakeIntrusive<TFeaturesLayout>(rawObjectsDataLayout);

        AddIgnoredFeatures(quantizedFeaturesInfoLayout, featuresLayout.Get());

        return featuresLayout;
    }


    // this is a helper class needed for friend declarations
    class TQuantizationImpl {
    public:
        // returns nullptr if generateBordersOnly
        static TQuantizedDataProviderPtr Do(
            const TQuantizationOptions& options,
            TRawDataProviderPtr rawDataProvider,
            TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
            bool calcQuantizationAndNanModeOnly,
            TRestorableFastRng64* rand,
            NPar::TLocalExecutor* localExecutor,
            const TInitialBorders& initialBorders = Nothing()
        ) {
             CB_ENSURE_INTERNAL(
                options.CpuCompatibleFormat || options.GpuCompatibleFormat,
                "TQuantizationOptions: at least one of CpuCompatibleFormat or GpuCompatibleFormat"
                "options must be true"
            );

            auto& srcObjectsCommonData = rawDataProvider->ObjectsData->CommonData;

            auto featuresLayout = InitFeaturesLayoutForQuantizedData(
                *srcObjectsCommonData.FeaturesLayout,
                *quantizedFeaturesInfo->GetFeaturesLayout()
            );

            const bool clearSrcData = rawDataProvider->RefCount() <= 1;
            const bool clearSrcObjectsData = clearSrcData &&
                (rawDataProvider->ObjectsData->RefCount() <= 1);

            const bool bundleExclusiveFeatures =
                options.CpuCompatibleFormat && options.BundleExclusiveFeaturesForCpu;

            /*
             * If these conditions are satisfied quantized features data is only needed for GPU
             *  so it is possible not to store all quantized features bins in CPU RAM
             *  but generate these quantized feature bin values from raw feature values on the fly
             *  just before copying data to GPU memory.
             *  Returned TQuantizedObjectsDataProvider will contain
             *  TExternalFloatValuesHolders and TExternalCatValuesHolders in features data holders.
             */
            const bool storeFeaturesDataAsExternalValuesHolders = !options.CpuCompatibleFormat &&
                !clearSrcObjectsData &&
                !featuresLayout->GetTextFeatureCount();

            TObjectsGroupingPtr objectsGrouping = rawDataProvider->ObjectsGrouping;

            TSubsetIndexingForBuildBorders subsetIndexingForBuildBorders = GetSubsetForBuildBorders(
                *srcObjectsCommonData.SubsetIndexing,
                *featuresLayout,
                *quantizedFeaturesInfo,
                srcObjectsCommonData.Order,
                options,
                rand,
                localExecutor
            );

            const bool hasDenseSrcData = rawDataProvider->ObjectsData->HasDenseData();

            TMaybe<TQuantizedForCPUBuilderData> data;
            TAtomicSharedPtr<TArraySubsetIndexing<ui32>> subsetIndexing;
            TMaybe<TIncrementalDenseIndexing> incrementalIndexing;

            if (!calcQuantizationAndNanModeOnly) {
                data.ConstructInPlace();

                auto flatFeatureCount = featuresLayout->GetExternalFeatureCount();
                data->ObjectsData.PackedBinaryFeaturesData.FlatFeatureIndexToPackedBinaryIndex.resize(
                    flatFeatureCount
                );
                data->ObjectsData.ExclusiveFeatureBundlesData.FlatFeatureIndexToBundlePart.resize(
                    flatFeatureCount
                );
                data->ObjectsData.FeaturesGroupsData.FlatFeatureIndexToGroupPart.resize(
                    flatFeatureCount
                );

                data->ObjectsData.Data.FloatFeatures.resize(featuresLayout->GetFloatFeatureCount());
                data->ObjectsData.Data.CatFeatures.resize(featuresLayout->GetCatFeatureCount());
                data->ObjectsData.Data.TextFeatures.resize(quantizedFeaturesInfo->GetTokenizedFeatureCount());

                if (storeFeaturesDataAsExternalValuesHolders) {
                    // external columns keep the same subset
                    subsetIndexing = srcObjectsCommonData.SubsetIndexing;
                } else {
                    subsetIndexing = MakeAtomicShared<TArraySubsetIndexing<ui32>>(
                        TFullSubset<ui32>(objectsGrouping->GetObjectCount())
                    );
                }

                incrementalIndexing.ConstructInPlace(
                    *srcObjectsCommonData.SubsetIndexing,
                    hasDenseSrcData,
                    localExecutor);
            }

            {
                ui64 cpuRamUsage = NMemInfo::GetMemInfo().RSS;
                OutputWarningIfCpuRamUsageOverLimit(cpuRamUsage, options.CpuRamLimit);

                TResourceConstrainedExecutor resourceConstrainedExecutor(
                    "CPU RAM",
                    options.CpuRamLimit - Min(cpuRamUsage, options.CpuRamLimit),
                    true,
                    localExecutor
                );

                const bool calcQuantizationAndNanModeOnlyInProcessFloatFeatures =
                    calcQuantizationAndNanModeOnly || bundleExclusiveFeatures;

                featuresLayout->IterateOverAvailableFeatures<EFeatureType::Float>(
                    [&] (TFloatFeatureIdx floatFeatureIdx) {
                        // as pointer to capture in lambda
                        auto* srcFloatFeatureHolderPtr =
                            &(rawDataProvider->ObjectsData->Data.FloatFeatures[*floatFeatureIdx]);

                        resourceConstrainedExecutor.Add(
                            {
                                EstimateMemUsageForFloatFeature(
                                    **srcFloatFeatureHolderPtr,
                                    *quantizedFeaturesInfo,
                                    options,
                                    !calcQuantizationAndNanModeOnly,
                                    storeFeaturesDataAsExternalValuesHolders
                                ),
                                [&, floatFeatureIdx, srcFloatFeatureHolderPtr]() {
                                    ProcessFloatFeature(
                                        floatFeatureIdx,
                                        **srcFloatFeatureHolderPtr,
                                        subsetIndexingForBuildBorders,
                                        options,
                                        initialBorders,
                                        calcQuantizationAndNanModeOnlyInProcessFloatFeatures,
                                        storeFeaturesDataAsExternalValuesHolders,
                                        incrementalIndexing,
                                        subsetIndexing.Get(),
                                        localExecutor,
                                        quantizedFeaturesInfo,
                                        calcQuantizationAndNanModeOnlyInProcessFloatFeatures ?
                                            nullptr
                                            : &(data->ObjectsData.Data.FloatFeatures[*floatFeatureIdx])
                                    );

                                    // exclusive features are bundled later by bundle,
                                    // binary features are binarized later by packs
                                    if (clearSrcObjectsData &&
                                        (calcQuantizationAndNanModeOnly ||
                                         (!bundleExclusiveFeatures &&
                                          !IsFloatFeatureToBeBinarized(
                                              options,
                                              *quantizedFeaturesInfo,
                                              floatFeatureIdx
                                         ))))
                                    {
                                        srcFloatFeatureHolderPtr->Destroy();
                                    }
                                }
                            }
                        );
                    }
                );

                if (!calcQuantizationAndNanModeOnly) {
                    featuresLayout->IterateOverAvailableFeatures<EFeatureType::Categorical>(
                         [&] (TCatFeatureIdx catFeatureIdx) {
                            // as pointer to capture in lambda
                            auto* srcCatFeatureHolderPtr =
                                &(rawDataProvider->ObjectsData->Data.CatFeatures[*catFeatureIdx]);

                            resourceConstrainedExecutor.Add(
                                {
                                    EstimateMemUsageForCatFeature(
                                        **srcCatFeatureHolderPtr,
                                        storeFeaturesDataAsExternalValuesHolders
                                    ),
                                    [&, catFeatureIdx, srcCatFeatureHolderPtr]() {
                                        ProcessCatFeature(
                                            catFeatureIdx,
                                            **srcCatFeatureHolderPtr,
                                            options,
                                            bundleExclusiveFeatures,
                                            storeFeaturesDataAsExternalValuesHolders,
                                            *incrementalIndexing,
                                            subsetIndexing.Get(),
                                            localExecutor,
                                            quantizedFeaturesInfo,
                                            &(data->ObjectsData.Data.CatFeatures[*catFeatureIdx])
                                        );

                                        // exclusive features are bundled later by bundle,
                                        // binary features are binarized later by packs
                                        if (clearSrcObjectsData &&
                                            (!bundleExclusiveFeatures &&
                                              !IsCatFeatureToBeBinarized(
                                                  options,
                                                  *quantizedFeaturesInfo,
                                                  catFeatureIdx
                                             )))
                                        {
                                            srcCatFeatureHolderPtr->Destroy();
                                        }
                                    }
                                }
                            );
                        }
                    );


                    CreateDictionaries(
                        MakeConstArrayRef(rawDataProvider->ObjectsData->Data.TextFeatures),
                        *quantizedFeaturesInfo->GetFeaturesLayout(),
                        quantizedFeaturesInfo->GetTextProcessingOptions(),
                        quantizedFeaturesInfo->GetTextDigitizersMutable()
                    );

                    ProcessTextFeatures(
                        rawDataProvider->ObjectsData->Data.TextFeatures,
                        subsetIndexing.Get(),
                        quantizedFeaturesInfo->GetTextDigitizers(),
                        data->ObjectsData.Data.TextFeatures,
                        localExecutor
                    );

                    AddTokenizedFeaturesToFeatureLayout(
                        quantizedFeaturesInfo->GetTextProcessingOptions(),
                        featuresLayout.Get()
                    );
                }

                resourceConstrainedExecutor.ExecTasks();
            }

            if (calcQuantizationAndNanModeOnly) {
                return nullptr;
            }

            // update after possibly updated quantizedFeaturesInfo
            AddIgnoredFeatures(*(quantizedFeaturesInfo->GetFeaturesLayout()), featuresLayout.Get());

            CB_ENSURE(
                featuresLayout->HasAvailableAndNotIgnoredFeatures(),
                "All features are either constant or ignored."
            );

            data->ObjectsData.Data.QuantizedFeaturesInfo = quantizedFeaturesInfo;


            if (bundleExclusiveFeatures) {
                data->ObjectsData.ExclusiveFeatureBundlesData = TExclusiveFeatureBundlesData(
                    *featuresLayout,
                    CreateExclusiveFeatureBundles(
                        rawDataProvider->ObjectsData->Data,
                        *incrementalIndexing,
                        *featuresLayout,
                        *(data->ObjectsData.Data.QuantizedFeaturesInfo),
                        options.ExclusiveFeaturesBundlingOptions,
                        localExecutor
                    )
                );
            }

            if (options.CpuCompatibleFormat && options.PackBinaryFeaturesForCpu) {
                data->ObjectsData.PackedBinaryFeaturesData = TPackedBinaryFeaturesData(
                    *featuresLayout,
                    *data->ObjectsData.Data.QuantizedFeaturesInfo,
                    data->ObjectsData.ExclusiveFeatureBundlesData
                );
            }
            if (options.CpuCompatibleFormat && options.GroupFeaturesForCpu) {
                data->ObjectsData.FeaturesGroupsData = TFeatureGroupsData(
                    *featuresLayout,
                    CreateFeatureGroups(
                        *featuresLayout,
                        *data->ObjectsData.Data.QuantizedFeaturesInfo,
                        data->ObjectsData.ExclusiveFeatureBundlesData.FlatFeatureIndexToBundlePart,
                        data->ObjectsData.PackedBinaryFeaturesData.FlatFeatureIndexToPackedBinaryIndex,
                        options.FeaturesGroupingOptions
                    )
                );
            }

            {
                TColumnsQuantizer quantizer(
                    clearSrcObjectsData,
                    options,
                    *incrementalIndexing,
                    *featuresLayout,
                    subsetIndexing.Get(),
                    localExecutor,
                    &rawDataProvider->ObjectsData->Data,
                    &data->ObjectsData
                );

                quantizer.Do();
            }

            if (clearSrcData) {
                data->MetaInfo = std::move(rawDataProvider->MetaInfo);
                data->TargetData = std::move(rawDataProvider->RawTargetData.Data);
                if (clearSrcObjectsData) {
                    data->CommonObjectsData = std::move(rawDataProvider->ObjectsData->CommonData);
                } else {
                    data->CommonObjectsData = rawDataProvider->ObjectsData->CommonData;
                }
            } else {
                data->MetaInfo = rawDataProvider->MetaInfo;
                data->TargetData = rawDataProvider->RawTargetData.Data;
                data->CommonObjectsData = rawDataProvider->ObjectsData->CommonData;
            }
            data->MetaInfo.FeaturesLayout = featuresLayout;
            data->CommonObjectsData.FeaturesLayout = featuresLayout;
            data->CommonObjectsData.SubsetIndexing = std::move(subsetIndexing);

            if (options.CpuCompatibleFormat) {
                return MakeDataProvider<TQuantizedForCPUObjectsDataProvider>(
                    objectsGrouping,
                    std::move(*data),
                    false,
                    localExecutor
                )->CastMoveTo<TQuantizedObjectsDataProvider>();
            } else {
                return MakeDataProvider<TQuantizedObjectsDataProvider>(
                    objectsGrouping,
                    CastToBase(std::move(*data)),
                    false,
                    localExecutor
                );
            }
        }
    };


    void CalcBordersAndNanMode(
        const TQuantizationOptions& options,
        TRawDataProviderPtr rawDataProvider,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
        TRestorableFastRng64* rand,
        NPar::TLocalExecutor* localExecutor
    ) {
        TQuantizationImpl::Do(
            options,
            std::move(rawDataProvider),
            quantizedFeaturesInfo,
            true,
            rand,
            localExecutor
        );
    }

    TQuantizedObjectsDataProviderPtr Quantize(
        const TQuantizationOptions& options,
        TRawObjectsDataProviderPtr rawObjectsDataProvider,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
        TRestorableFastRng64* rand,
        NPar::TLocalExecutor* localExecutor,
        const TInitialBorders& initialBorders
    ) {
        TDataMetaInfo dataMetaInfo;
        dataMetaInfo.FeaturesLayout = rawObjectsDataProvider->GetFeaturesLayout();

        auto objectsGrouping = rawObjectsDataProvider->GetObjectsGrouping();

        TRawTargetData dummyData;
        dummyData.SetTrivialWeights(rawObjectsDataProvider->GetObjectCount());

        auto rawDataProvider = MakeIntrusive<TRawDataProvider>(
            std::move(dataMetaInfo),
            std::move(rawObjectsDataProvider),
            objectsGrouping,
            TRawTargetDataProvider(objectsGrouping, std::move(dummyData), true, nullptr)
        );

        auto quantizedDataProvider = Quantize(
            options,
            std::move(rawDataProvider),
            quantizedFeaturesInfo,
            rand,
            localExecutor,
            initialBorders
        );

        return quantizedDataProvider->ObjectsData;
    }


    TQuantizedDataProviderPtr Quantize(
        const TQuantizationOptions& options,
        TRawDataProviderPtr rawDataProvider,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
        TRestorableFastRng64* rand,
        NPar::TLocalExecutor* localExecutor,
        const TInitialBorders& initialBorders
    ) {
        return TQuantizationImpl::Do(
            options,
            std::move(rawDataProvider),
            quantizedFeaturesInfo,
            false,
            rand,
            localExecutor,
            initialBorders
        );
    }

    TQuantizedDataProviders Quantize(
        const TQuantizationOptions& options,
        const NCatboostOptions::TDataProcessingOptions& dataProcessingOptions,
        bool floatFeaturesAllowNansInTestOnly,
        TConstArrayRef<ui32> ignoredFeatures,
        TRawDataProviders rawDataProviders,
        TRestorableFastRng64* rand,
        NPar::TLocalExecutor* localExecutor
    ) {
        TQuantizedDataProviders result;
        auto quantizedFeaturesInfo = MakeIntrusive<TQuantizedFeaturesInfo>(
            *rawDataProviders.Learn->MetaInfo.FeaturesLayout,
            ignoredFeatures,
            dataProcessingOptions.FloatFeaturesBinarization.Get(),
            dataProcessingOptions.PerFloatFeatureQuantization.Get(),
            dataProcessingOptions.TextProcessingOptions.Get(),
            floatFeaturesAllowNansInTestOnly,
            options.AllowWriteFiles
        );

        result.Learn = Quantize(
            options,
            std::move(rawDataProviders.Learn),
            quantizedFeaturesInfo,
            rand,
            localExecutor
        );

        // TODO(akhropov): quantize test data in parallel
        for (auto& rawTestData : rawDataProviders.Test) {
            result.Test.push_back(
                Quantize(options, std::move(rawTestData), quantizedFeaturesInfo, rand, localExecutor)
            );
        }

        return result;
    }


    TQuantizedObjectsDataProviderPtr GetQuantizedObjectsData(
        const NCatboostOptions::TCatBoostOptions& params,
        TDataProviderPtr srcData,
        const TMaybe<TString>& bordersFile,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo,
        bool allowWriteFiles,
        NPar::TLocalExecutor* localExecutor,
        TRestorableFastRng64* rand,
        const TInitialBorders& initialBorders) {

        TQuantizationOptions quantizationOptions;
        quantizationOptions.GroupFeaturesForCpu = params.DataProcessingOptions->DevGroupFeatures.GetUnchecked();
        if (params.GetTaskType() == ETaskType::CPU) {
            quantizationOptions.GpuCompatibleFormat = false;

            quantizationOptions.ExclusiveFeaturesBundlingOptions.MaxBuckets
                = params.ObliviousTreeOptions->DevExclusiveFeaturesBundleMaxBuckets.Get();
            quantizationOptions.ExclusiveFeaturesBundlingOptions.MaxConflictFraction
                = params.ObliviousTreeOptions->SparseFeaturesConflictFraction.Get();

            /* TODO(kirillovs): Sparse features support for GPU
             * TODO(akhropov): Enable when sparse column scoring is supported

            float defaultValueFractionToEnableSparseStorage
                = params.DataProcessingOptions->DevDefaultValueFractionToEnableSparseStorage.Get();
            if (defaultValueFractionToEnableSparseStorage > 0.0f) {
                quantizationOptions.DefaultValueFractionToEnableSparseStorage
                    = defaultValueFractionToEnableSparseStorage;
                quantizationOptions.SparseArrayIndexingType
                    = params.DataProcessingOptions->DevSparseArrayIndexingType.Get();
            }
            */
        } else {
            Y_ASSERT(params.GetTaskType() == ETaskType::GPU);

            /*
             * if there're any cat features format should be CPU-compatible to enable final CTR
             * calculations.
             * TODO(akhropov): compatibility with final CTR calculation should not depend on this flag
             */
            quantizationOptions.CpuCompatibleFormat
                = srcData->MetaInfo.FeaturesLayout->GetCatFeatureCount() != 0;
            if (quantizationOptions.CpuCompatibleFormat) {
                /* don't spend time on bundling preprocessing because it won't be used
                 *
                 * TODO(akhropov): maybe there are cases where CPU RAM usage reduction is more important
                 *    than calculation speed so it should be enabled
                 */
                quantizationOptions.BundleExclusiveFeaturesForCpu = false;

                // grouping is unused on GPU
                quantizationOptions.GroupFeaturesForCpu = false;
            }
        }
        quantizationOptions.CpuRamLimit
            = ParseMemorySizeDescription(params.SystemOptions->CpuUsedRamLimit.Get());
        quantizationOptions.AllowWriteFiles = allowWriteFiles;

        if (!quantizedFeaturesInfo) {
            quantizedFeaturesInfo = MakeIntrusive<TQuantizedFeaturesInfo>(
                *srcData->MetaInfo.FeaturesLayout,
                params.DataProcessingOptions->IgnoredFeatures.Get(),
                params.DataProcessingOptions->FloatFeaturesBinarization.Get(),
                params.DataProcessingOptions->PerFloatFeatureQuantization.Get(),
                params.DataProcessingOptions->TextProcessingOptions.Get(),
                /*allowNansInTestOnly*/true,
                allowWriteFiles
            );

            if (bordersFile) {
                LoadBordersAndNanModesFromFromFileInMatrixnetFormat(
                    *bordersFile,
                    quantizedFeaturesInfo.Get());
            }
        }

        TRawObjectsDataProviderPtr rawObjectsDataProvider(
            dynamic_cast<TRawObjectsDataProvider*>(srcData->ObjectsData.Get()));
        Y_VERIFY(rawObjectsDataProvider);

        if (srcData->RefCount() <= 1) {
            // can clean up
            auto dummy = srcData->ObjectsData.Release();
            Y_UNUSED(dummy);
        }

        return Quantize(
            quantizationOptions,
            std::move(rawObjectsDataProvider),
            quantizedFeaturesInfo,
            rand,
            localExecutor,
            initialBorders
        );
    }


    TQuantizedObjectsDataProviderPtr ConstructQuantizedPoolFromRawPool(
        TDataProviderPtr srcData,
        NJson::TJsonValue plainJsonParams,
        TQuantizedFeaturesInfoPtr quantizedFeaturesInfo
    ) {

        NJson::TJsonValue jsonParams;
        NJson::TJsonValue outputJsonParams;
        ConvertIgnoredFeaturesFromStringToIndices(srcData.Get()->MetaInfo, &plainJsonParams);
        NCatboostOptions::PlainJsonToOptions(plainJsonParams, &jsonParams, &outputJsonParams);
        NCatboostOptions::TCatBoostOptions catBoostOptions(NCatboostOptions::LoadOptions(jsonParams));
        NCatboostOptions::TOutputFilesOptions outputFileOptions;
        outputFileOptions.Load(outputJsonParams);

        const bool allowWriteFiles = outputFileOptions.AllowWriteFiles();

        const ui32 allDataObjectCount = srcData->ObjectsData->GetObjectCount();

        CB_ENSURE(allDataObjectCount != 0, "Pool is empty");

        TRestorableFastRng64 rand(catBoostOptions.RandomSeed.Get());

        NPar::TLocalExecutor localExecutor;
        localExecutor.RunAdditionalThreads(catBoostOptions.SystemOptions->NumThreads.Get() - 1);

        TLabelConverter labelConverter;

        return GetQuantizedObjectsData(
            catBoostOptions,
            srcData,
            Nothing(),
            quantizedFeaturesInfo,
            allowWriteFiles,
            &localExecutor,
            &rand);
    }


}
