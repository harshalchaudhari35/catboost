#include "eval_feature.h"
#include "train_model.h"
#include "options_helper.h"

#include <catboost/private/libs/algo/apply.h>
#include <catboost/private/libs/algo/approx_dimension.h>
#include <catboost/private/libs/algo/data.h>
#include <catboost/private/libs/algo/helpers.h>
#include <catboost/private/libs/algo/preprocess.h>
#include <catboost/private/libs/algo/train.h>
#include <catboost/libs/fstr/output_fstr.h>
#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/helpers/parallel_tasks.h>
#include <catboost/libs/helpers/progress_helper.h>
#include <catboost/libs/helpers/restorable_rng.h>
#include <catboost/libs/helpers/vector_helpers.h>
#include <catboost/libs/helpers/wx_test.h>
#include <catboost/libs/loggers/catboost_logger_helpers.h>
#include <catboost/libs/loggers/logger.h>
#include <catboost/libs/logging/logging.h>
#include <catboost/libs/logging/profile_info.h>
#include <catboost/libs/metrics/metric.h>
#include <catboost/libs/model/features.h>
#include <catboost/libs/model/model.h>
#include <catboost/private/libs/options/enum_helpers.h>
#include <catboost/private/libs/options/feature_eval_options.h>
#include <catboost/private/libs/options/output_file_options.h>
#include <catboost/private/libs/options/plain_options_helper.h>

#include <util/generic/algorithm.h>
#include <util/generic/array_ref.h>
#include <util/generic/scope.h>
#include <util/generic/xrange.h>
#include <util/generic/ymath.h>
#include <util/generic/maybe.h>
#include <util/generic/utility.h>
#include <util/stream/labeled.h>
#include <util/string/cast.h>
#include <util/string/join.h>
#include <util/string/builder.h>
#include <util/system/hp_timer.h>
#include <util/ysaveload.h>

#include <cmath>
#include <numeric>


using namespace NCB;


TString ToString(const TFeatureEvaluationSummary& summary) {
    TString featureEvalTsv;

    TStringOutput featureEvalStream(featureEvalTsv);
    featureEvalStream << "p-value\tbest iteration in each fold\t";
    for (const auto& metricName : summary.MetricNames) {
        featureEvalStream << metricName << '\t';
    }
    featureEvalStream << "feature set" << Endl;
    for (ui32 featureSetIdx : xrange(summary.FeatureSets.size())) {
        featureEvalStream << summary.WxTest[featureSetIdx] << '\t';
        const auto& bestIterations = summary.BestBaselineIterations[featureSetIdx];
        featureEvalStream << JoinRange(",", bestIterations.begin(), bestIterations.end());
        featureEvalStream << '\t';
        for (double delta : summary.AverageMetricDelta[featureSetIdx]) {
            featureEvalStream << delta << '\t';
        }
        const auto& featureSet = summary.FeatureSets[featureSetIdx];
        featureEvalStream << JoinRange(",", featureSet.begin(), featureSet.end());
        featureEvalStream << Endl;
    }
    return featureEvalTsv;
}


static TVector<EMetricBestValue> GetBestValueType(
    const TVector<THolder<IMetric>>& metrics
) {
    TVector<EMetricBestValue> bestValueType;
    for (const auto& metric : metrics) {
        EMetricBestValue valueType;
        float bestValue;
        metric->GetBestValue(&valueType, &bestValue);
        CB_ENSURE(
            EqualToOneOf(valueType, EMetricBestValue::Min, EMetricBestValue::Max),
            "Metric " + metric->GetDescription() + " has neither lower, nor upper bound"
        );
        bestValueType.push_back(valueType);
    }
    return bestValueType;
}

static ui32 GetBestIterationInFold(
    const TVector<EMetricBestValue>& bestValueType,
    const TVector<TVector<double>>& metricValues // [iterIdx][metricIdx]
) {
    ui32 bestIteration = 0;
    constexpr ui32 lossIdx = 0;
    for (ui32 iteration = 1; iteration < metricValues.size(); ++iteration) {
        if (bestValueType[lossIdx] == EMetricBestValue::Min) {
            if (metricValues[iteration][lossIdx] < metricValues[bestIteration][lossIdx]) {
                bestIteration = iteration;
            }
        } else {
            if (metricValues[iteration][lossIdx] > metricValues[bestIteration][lossIdx]) {
                bestIteration = iteration;
            }
        }
    }
    return bestIteration;
}


void TFeatureEvaluationSummary::AppendFeatureSetMetrics(
    bool isTest,
    ui32 featureSetIdx,
    const TVector<TVector<double>>& metricValuesOnFold
) {
    const auto featureSetCount = FeatureSets.size();
    CB_ENSURE_INTERNAL(featureSetIdx < featureSetCount, "Feature set index is too large");
    const ui32 bestIteration = GetBestIterationInFold(MetricTypes, metricValuesOnFold);
    if (!isTest) {
        BestBaselineIterations[featureSetIdx].push_back(bestIteration);
    }
    auto& bestMetrics = BestMetrics[isTest];
    auto& featureSetBestMetrics = bestMetrics[featureSetIdx];
    const auto metricCount = MetricTypes.size();
    featureSetBestMetrics.resize(metricCount);
    for (auto metricIdx : xrange(metricCount)) {
        const double bestMetric = metricValuesOnFold[bestIteration][metricIdx];
        featureSetBestMetrics[metricIdx].push_back(bestMetric);
    }
}


void TFeatureEvaluationSummary::CalcWxTestAndAverageDelta() {
    const auto featureSetCount = FeatureSets.size();
    const auto metricCount = MetricTypes.size();
    TVector<double> averageDelta(metricCount);
    WxTest.resize(featureSetCount);
    AverageMetricDelta.resize(featureSetCount);
    constexpr ui32 LossIdx = 0;
    for (auto featureSetIdx : xrange(featureSetCount)) {
        const auto& baselineMetrics = BestMetrics[/*isTest*/0][featureSetIdx];
        const auto& testedMetrics = BestMetrics[/*isTest*/1][featureSetIdx];
        WxTest[featureSetIdx] = ::WxTest(baselineMetrics[LossIdx], testedMetrics[LossIdx]).PValue;

        const auto foldCount = baselineMetrics.size();
        for (auto metricIdx : xrange(metricCount)) {
            const auto baselineAverage = Accumulate(baselineMetrics[metricIdx], 0.0) / foldCount;
            const auto testedAverage = Accumulate(testedMetrics[metricIdx], 0.0) / foldCount;
            if (MetricTypes[metricIdx] == EMetricBestValue::Min) {
                averageDelta[metricIdx] = - testedAverage + baselineAverage;
            } else {
                averageDelta[metricIdx] = + testedAverage - baselineAverage;
            }
        }
        AverageMetricDelta[featureSetIdx] = averageDelta;
    }
}


static void CreateLogFromHistory(
    const NCatboostOptions::TOutputFilesOptions& outputFileOptions,
    const TVector<THolder<IMetric>>& metrics,
    const TMetricsAndTimeLeftHistory& metricsHistory,
    ui32 iterationCount,
    TLogger* logger
) {
    const TVector<bool> skipMetricOnTrain = GetSkipMetricOnTrain(metrics);
    const TString learnToken = "learn";
    const TString testToken = "test";
    CB_ENSURE_INTERNAL(
        outputFileOptions.GetMetricPeriod() == 1,
        "Feature evaluation requires metric_period=1");
    constexpr int errorTrackerMetricIdx = 0;
    for (ui32 iteration = 0; iteration < iterationCount; ++iteration) {
        TOneInterationLogger oneIterLogger(*logger);
        for (int metricIdx = 0; metricIdx < metrics.ysize(); ++metricIdx) {
            const auto& metric = metrics[metricIdx];
            const auto& metricDescription = metric->GetDescription();
            if (!skipMetricOnTrain[metricIdx]) {
                const double metricOnLearn = metricsHistory.LearnMetricsHistory[iteration].at(metricDescription);
                oneIterLogger.OutputMetric(
                    learnToken,
                    TMetricEvalResult(metricDescription, metricOnLearn, metricIdx == errorTrackerMetricIdx)
                );
            }
            const double metricOnTest = metricsHistory.TestMetricsHistory[iteration][0].at(metricDescription);
            oneIterLogger.OutputMetric(
                testToken,
                TMetricEvalResult(metricDescription, metricOnTest, metricIdx == errorTrackerMetricIdx)
            );
        }
    }
}

static TString MakeFoldDirName(
    const NCatboostOptions::TFeatureEvalOptions& featureEvalOptions,
    bool isTest,
    ui32 featureSetIdx,
    ui32 foldIdx
) {
    auto foldDir = TStringBuilder();
    if (!isTest) {
        foldDir << "Baseline_";
        const auto evalMode = featureEvalOptions.FeatureEvalMode;
        const auto featureSetCount = featureEvalOptions.FeaturesToEvaluate->size();
        if (featureSetCount > 0 && evalMode == NCB::EFeatureEvalMode::OneVsOthers) {
            foldDir << "set_" << featureSetIdx << "_";
        }
    } else {
        foldDir << "Testing_set_" << featureSetIdx << "_";
    }
    foldDir << "fold_" << foldIdx;
    return foldDir;
}

void TFeatureEvaluationSummary::CreateLogs(
    const NCatboostOptions::TOutputFilesOptions& outputFileOptions,
    const NCatboostOptions::TFeatureEvalOptions& featureEvalOptions,
    const TVector<THolder<IMetric>>& metrics,
    ui32 iterationCount,
    bool isTest,
    ui32 foldRangeBegin,
    ui32 absoluteOffset
) {
    const ui32 featureSetCount = FeatureSets.size();
    const auto topLevelTrainDir = outputFileOptions.GetTrainDir();
    const auto& metricsHistory = MetricsHistory[isTest];
    const auto& featureStrengths = FeatureStrengths[isTest];
    const auto& regularFeatureStrengths = RegularFeatureStrengths[isTest];
    const auto& metricsMetaJson = GetJsonMeta(
        iterationCount,
        outputFileOptions.GetName(),
        GetConstPointers(metrics),
        /*learnSetNames*/{"learn"},
        /*testSetNames*/{"test"},
        /*parametersName=*/ "",
        ELaunchMode::CV);
    const ui32 absoluteBegin = foldRangeBegin + featureEvalOptions.Offset;
    const ui32 absoluteEnd = absoluteBegin + featureEvalOptions.FoldCount;
    const bool useSetZeroAlways = !isTest && featureEvalOptions.FeatureEvalMode != NCB::EFeatureEvalMode::OneVsOthers;
    for (ui32 setIdx : xrange(featureSetCount)) {
        for (ui32 absoluteFoldIdx : xrange(absoluteBegin, absoluteEnd)) {
            const auto foldDir = MakeFoldDirName(
                featureEvalOptions,
                isTest,
                setIdx,
                absoluteFoldIdx);
            auto options = outputFileOptions;
            options.SetTrainDir(JoinFsPaths(topLevelTrainDir, foldDir));
            TLogger logger;
            InitializeFileLoggers(
                options,
                metricsMetaJson,
                /*namesPrefix*/"",
                /*isDetailedProfile*/false,
                &logger);
            CreateLogFromHistory(
                options,
                metrics,
                metricsHistory[useSetZeroAlways ? 0 : setIdx][absoluteFoldIdx - absoluteOffset],
                iterationCount,
                &logger
            );
            const auto fstrPath = options.CreateFstrIternalFullPath();
            if (!fstrPath.empty()) {
                OutputStrengthDescriptions(
                    featureStrengths[useSetZeroAlways ? 0 : setIdx][absoluteFoldIdx - absoluteOffset],
                    fstrPath);
            }
            const auto regularFstrPath = options.CreateFstrRegularFullPath();
            if (!regularFstrPath.empty()) {
                OutputStrengthDescriptions(
                    regularFeatureStrengths[useSetZeroAlways ? 0 : setIdx][absoluteFoldIdx - absoluteOffset],
                    regularFstrPath);
            }
        }
    }
}


bool TFeatureEvaluationSummary::HasHeaderInfo() const {
    return !MetricNames.empty();
}


void TFeatureEvaluationSummary::SetHeaderInfo(
    const TVector<THolder<IMetric>>& metrics,
    const TVector<TVector<ui32>>& featureSets
) {
    MetricTypes = GetBestValueType(metrics);
    MetricNames.clear();
    for (const auto& metric : metrics) {
        MetricNames.push_back(metric->GetDescription());
    }
    FeatureSets = featureSets;
    const ui32 featureSetCount = featureSets.size();
    ResizeRank2(2, featureSetCount, MetricsHistory);
    ResizeRank2(2, featureSetCount, FeatureStrengths);
    ResizeRank2(2, featureSetCount, RegularFeatureStrengths);
    ResizeRank2(2, featureSetCount, BestMetrics);
    BestBaselineIterations.resize(featureSetCount);
}


static bool IsObjectwiseEval(const NCatboostOptions::TFeatureEvalOptions& featureEvalOptions) {
    return featureEvalOptions.FoldSizeUnit.Get() == ESamplingUnit::Object;
}

template <class TDataProvidersTemplate> // TTrainingDataProvidersTemplate<...>
static void PrepareFolds(
    typename TDataProvidersTemplate::TDataPtr srcData,
    const TCvDataPartitionParams& cvParams,
    const NCatboostOptions::TFeatureEvalOptions& featureEvalOptions,
    ui64 cpuUsedRamLimit,
    TVector<TDataProvidersTemplate>* foldsData,
    TVector<TDataProvidersTemplate>* testFoldsData,
    NPar::TLocalExecutor* localExecutor
) {
    const int foldCount = cvParams.Initialized() ? cvParams.FoldCount : featureEvalOptions.FoldCount.Get();
    CB_ENSURE(foldCount > 0, "Fold count must be positive integer");
    const auto& objectsGrouping = *srcData->ObjectsGrouping;
    TVector<NCB::TArraySubsetIndexing<ui32>> testSubsets;
    if (cvParams.Initialized()) {
        // group subsets, groups maybe trivial
        testSubsets = NCB::Split(objectsGrouping, foldCount);
        // always inverted
        CB_ENSURE(cvParams.Type == ECrossValidation::Inverted, "Feature evaluation requires inverted cross-validation");
    } else {
        const ui32 foldSize = featureEvalOptions.FoldSize;
        CB_ENSURE(foldSize > 0, "Fold size must be positive integer");
        // group subsets, groups maybe trivial
        const auto isObjectwise = IsObjectwiseEval(featureEvalOptions);
        testSubsets = isObjectwise
            ? NCB::SplitByObjects(objectsGrouping, foldSize)
            : NCB::SplitByGroups(objectsGrouping, foldSize);
        const ui32 offsetInRange = featureEvalOptions.Offset;
        CB_ENSURE_INTERNAL(offsetInRange + foldCount <= testSubsets.size(), "Dataset permutation logic failed");
    }
    // group subsets, maybe trivial
    TVector<NCB::TArraySubsetIndexing<ui32>> trainSubsets
        = CalcTrainSubsets(testSubsets, objectsGrouping.GetGroupCount());

    testSubsets.swap(trainSubsets);

    CB_ENSURE(foldsData->empty(), "Need empty vector of folds data");
    foldsData->resize(trainSubsets.size());
    if (testFoldsData != nullptr) {
        CB_ENSURE(testFoldsData->empty(), "Need empty vector of test folds data");
        testFoldsData->resize(trainSubsets.size());
    } else {
        testFoldsData = foldsData;
    }

    TVector<std::function<void()>> tasks;

    // NCB::Split keeps objects order
    const NCB::EObjectsOrder objectsOrder = NCB::EObjectsOrder::Ordered;

    const ui64 perTaskCpuUsedRamLimit = cpuUsedRamLimit / (2 * trainSubsets.size());

    for (ui32 foldIdx : xrange(trainSubsets.size())) {
        tasks.emplace_back(
            [&, foldIdx]() {
                (*foldsData)[foldIdx].Learn = srcData->GetSubset(
                    GetSubset(
                        srcData->ObjectsGrouping,
                        std::move(trainSubsets[foldIdx]),
                        objectsOrder
                    ),
                    perTaskCpuUsedRamLimit,
                    localExecutor
                );
            }
        );
        tasks.emplace_back(
            [&, foldIdx]() {
                (*testFoldsData)[foldIdx].Test.emplace_back(
                    srcData->GetSubset(
                        GetSubset(
                            srcData->ObjectsGrouping,
                            std::move(testSubsets[foldIdx]),
                            objectsOrder
                        ),
                        perTaskCpuUsedRamLimit,
                        localExecutor
                    )
                );
            }
        );
    }

    NCB::ExecuteTasksInParallel(&tasks, localExecutor);

    if (!cvParams.Initialized()) {
        const ui32 offsetInRange = featureEvalOptions.Offset;
        TVector<TDataProvidersTemplate>(foldsData->begin() + offsetInRange, foldsData->end()).swap(*foldsData);
        foldsData->resize(foldCount);
        if (testFoldsData != foldsData) {
            TVector<TDataProvidersTemplate>(testFoldsData->begin() + offsetInRange, testFoldsData->end()).swap(*testFoldsData);
            testFoldsData->resize(foldCount);
        }
    }
}


enum class ETrainingKind {
    Baseline,
    Testing
};


template<>
void Out<ETrainingKind>(IOutputStream& stream, ETrainingKind kind) {
     if (kind == ETrainingKind::Baseline) {
         stream << "Baseline";
         return;
     }
     if (kind == ETrainingKind::Testing) {
         stream << "Testing";
         return;
     }
     Y_UNREACHABLE();
}


template <typename TObjectsDataProvider> // TQuantizedForCPUObjectsDataProvider or TQuantizedObjectsDataProvider
TIntrusivePtr<TTrainingDataProvider> MakeFeatureSubsetDataProvider(
    const TVector<ui32>& ignoredFeatures,
    NCB::TTrainingDataProviderPtr trainingDataProvider
) {
    TIntrusivePtr<TObjectsDataProvider> newObjects = dynamic_cast<TObjectsDataProvider*>(
        trainingDataProvider->ObjectsData->GetFeaturesSubset(ignoredFeatures, &NPar::LocalExecutor()).Get());
    CB_ENSURE(
        newObjects,
        "Objects data provider must be TQuantizedForCPUObjectsDataProvider or TQuantizedObjectsDataProvider");
    TDataMetaInfo newMetaInfo = trainingDataProvider->MetaInfo;
    newMetaInfo.FeaturesLayout = newObjects->GetFeaturesLayout();
    return MakeIntrusive<TTrainingDataProvider>(
        TDataMetaInfo(newMetaInfo),
        trainingDataProvider->ObjectsGrouping,
        newObjects,
        trainingDataProvider->TargetData);
}


static TVector<TTrainingDataProviders> UpdateIgnoredFeaturesInLearn(
    ETaskType taskType,
    const NCatboostOptions::TFeatureEvalOptions& options,
    ETrainingKind trainingKind,
    ui32 testedFeatureSetIdx,
    const TVector<TTrainingDataProviders>& foldsData
) {
    TVector<ui32> ignoredFeatures;
    const auto& testedFeatures = options.FeaturesToEvaluate.Get();
    const auto featureEvalMode = options.FeatureEvalMode;
    if (trainingKind == ETrainingKind::Testing) {
        for (ui32 featureSetIdx : xrange(testedFeatures.size())) {
            if (featureSetIdx != testedFeatureSetIdx) {
                ignoredFeatures.insert(
                    ignoredFeatures.end(),
                    testedFeatures[featureSetIdx].begin(),
                    testedFeatures[featureSetIdx].end());
            }
        }
    } else if (featureEvalMode == NCB::EFeatureEvalMode::OneVsAll) {
        // no additional ignored features
    } else if (featureEvalMode == NCB::EFeatureEvalMode::OneVsOthers) {
        ignoredFeatures = testedFeatures[testedFeatureSetIdx];
    } else {
        CB_ENSURE(
            featureEvalMode == NCB::EFeatureEvalMode::OneVsNone,
            "Unknown feature evaluation mode " + ToString(featureEvalMode)
        );
        for (const auto& featureSet : testedFeatures) {
            ignoredFeatures.insert(
                ignoredFeatures.end(),
                featureSet.begin(),
                featureSet.end());
        }
    }
    TVector<TTrainingDataProviders> result;
    result.reserve(foldsData.size());
    if (taskType == ETaskType::CPU) {
        for (const auto& foldData : foldsData) {
            TTrainingDataProviders newTrainingData;
            newTrainingData.Learn = MakeFeatureSubsetDataProvider<TQuantizedForCPUObjectsDataProvider>(
                ignoredFeatures,
                foldData.Learn);
            newTrainingData.Test.push_back(
                MakeFeatureSubsetDataProvider<TQuantizedForCPUObjectsDataProvider>(
                    ignoredFeatures,
                    foldData.Test[0])
            );
            result.push_back(newTrainingData);
        }
    } else {
        for (const auto& foldData : foldsData) {
            TTrainingDataProviders newTrainingData;
            newTrainingData.Learn = MakeFeatureSubsetDataProvider<TQuantizedObjectsDataProvider>(
                ignoredFeatures,
                foldData.Learn);
            newTrainingData.Test.push_back(
                MakeFeatureSubsetDataProvider<TQuantizedObjectsDataProvider>(
                    ignoredFeatures,
                    foldData.Test[0])
            );
            result.push_back(newTrainingData);
        }
    }
    return result;
}


static void LoadOptions(
    const NJson::TJsonValue& plainJsonParams,
    NCatboostOptions::TCatBoostOptions* catBoostOptions,
    NCatboostOptions::TOutputFilesOptions* outputFileOptions
) {
    NJson::TJsonValue jsonParams;
    NJson::TJsonValue outputJsonParams;
    NCatboostOptions::PlainJsonToOptions(plainJsonParams, &jsonParams, &outputJsonParams);
    catBoostOptions->Load(jsonParams);
    outputFileOptions->Load(outputJsonParams);

    if (outputFileOptions->GetMetricPeriod() > 1) {
        CATBOOST_WARNING_LOG << "Warning: metric_period is ignored because "
            "feature evaluation needs metric values on each iteration" << Endl;
        outputFileOptions->SetMetricPeriod(1);
    }
}


static void CalcMetricsForTest(
    const TVector<THolder<IMetric>>& metrics,
    ui32 approxDimension,
    TTrainingDataProviders::TTrainingDataProviderTemplatePtr testData,
    TFoldContext* foldContext
) {
    CB_ENSURE_INTERNAL(
        foldContext->FullModel.Defined(), "No model in fold " << foldContext->FoldIdx);
    const auto treeCount = foldContext->FullModel->GetTreeCount();
    const ui32 iterationCount = foldContext->MetricValuesOnTrain.size();
    CB_ENSURE_INTERNAL(
        iterationCount == treeCount,
        "Fold " << foldContext->FoldIdx << ": model size (" << treeCount <<
        ") differs from iteration count (" << iterationCount << ")");

    const auto metricCount = metrics.size();
    auto& metricValuesOnTest = foldContext->MetricValuesOnTest;
    ResizeRank2(treeCount, metricCount, metricValuesOnTest);

    const auto docCount = testData->GetObjectCount();
    TVector<TVector<double>> approx;
    ResizeRank2(approxDimension, docCount, approx);
    TVector<TVector<double>> partialApprox;
    ResizeRank2(approxDimension, docCount, partialApprox);
    TVector<double> flatApproxBuffer;
    flatApproxBuffer.yresize(docCount * approxDimension);

    TModelCalcerOnPool modelCalcer(
        foldContext->FullModel.GetRef(),
        testData->ObjectsData,
        &NPar::LocalExecutor());
    for (auto treeIdx : xrange(treeCount)) {
        // TODO(kirillovs):
        //     apply (1) all models to the entire dataset on CPU or (2) GPU,
        // TODO(espetrov):
        //     calculate error for each model,
        //     error on test fold idx = error on entire dataset for model idx - error on learn fold idx
        //     refactor using the Visitor pattern
        modelCalcer.ApplyModelMulti(
            EPredictionType::RawFormulaVal,
            treeIdx,
            treeIdx + 1,
            &flatApproxBuffer,
            &partialApprox);
        for (auto dimensionIdx : xrange(approxDimension)) {
            for (auto docIdx : xrange(docCount)) {
                approx[dimensionIdx][docIdx] += partialApprox[dimensionIdx][docIdx];
            }
        }
        for (auto metricIdx : xrange(metricCount)) {
            metricValuesOnTest[treeIdx][metricIdx] = CalcMetric(
                *metrics[metricIdx],
                testData->TargetData,
                approx,
                &NPar::LocalExecutor()
            );
        }
    }
}


class TFeatureEvaluationCallbacks : public ITrainingCallbacks {
public:
    TFeatureEvaluationCallbacks(
        ui32 iterationCount,
        const NCatboostOptions::TFeatureEvalOptions& evalFeatureOptions,
        TFeatureEvaluationSummary* summary)
    : IterationCount(iterationCount)
    , EvalFeatureOptions(evalFeatureOptions)
    , Summary(summary)
    {
    }

    bool IsContinueTraining(const TMetricsAndTimeLeftHistory& history) override {
        ++IterationIdx;
        if (IterationIdx == IterationCount) {
            auto& foldsFromHistory = Summary->MetricsHistory[*IsTest][*FeatureSetIndex];
            const ui32 absoluteFoldIdx = *FoldRangeBegin + *FoldIndex;
            if (foldsFromHistory.size() > absoluteFoldIdx - GetAbsoluteOffset()) {
                CATBOOST_INFO_LOG << "Snapshot already contains metrics for fold " << absoluteFoldIdx << Endl;
            } else {
                CB_ENSURE_INTERNAL(
                    foldsFromHistory.size() == absoluteFoldIdx - GetAbsoluteOffset(),
                    "No metrics for fold " << absoluteFoldIdx - 1);
                foldsFromHistory.emplace_back(history);
            }
        }
        constexpr double HeartbeatSeconds = 1;
        if (TrainTimer.Passed() > HeartbeatSeconds) {
            TSetLogging infomationMode(ELoggingLevel::Info);
            CATBOOST_INFO_LOG << "Train iteration " << IterationIdx << " of " << IterationCount << Endl;
            TrainTimer.Reset();
        }
        return /*continue training*/true;
    }

    void OnSaveSnapshot(IOutputStream* snapshot) override {
        Summary->Save(snapshot);
        NJson::TJsonValue options;
        EvalFeatureOptions.Save(&options);
        ::SaveMany(snapshot, FoldRangeBegin, FeatureSetIndex, IsTest, FoldIndex, options);
    }

    bool OnLoadSnapshot(IInputStream* snapshot) override {
        if (!IsNextLoadValid) {
            return false;
        }
        Summary->Load(snapshot);
        NJson::TJsonValue options;
        ::LoadMany(snapshot, FoldRangeBegin, FeatureSetIndex, IsTest, FoldIndex, options);
        NCatboostOptions::TFeatureEvalOptions evalFeatureOptions;
        evalFeatureOptions.Load(options);
        CB_ENSURE(evalFeatureOptions == EvalFeatureOptions, "Current feaure evaluation options differ from options in snapshot");
        EvalFeatureOptions = evalFeatureOptions;
        IsNextLoadValid = false;
        return true;
    }

    void ResetIterationIndex() {
        IterationIdx = 0;
    }

    void LoadSnapshot(ETaskType taskType, const TString& snapshotFile) {
        TProgressHelper progressHelper(ToString(taskType));
        IsNextLoadValid = true;
        progressHelper.CheckedLoad(
            snapshotFile,
            [&](TIFStream* input) {
                OnLoadSnapshot(input);
            });
        IsNextLoadValid = true;
    }

    bool HaveTrainResultsInSnapshot(ui32 foldRangeBegin, ui32 featureSetIdx, bool isTest, ui32 foldIdx) {
        if (!IsNextLoadValid) {
            return false;
        }
        CB_ENSURE_INTERNAL(
            FoldRangeBegin.Defined() && FeatureSetIndex.Defined() && IsTest.Defined() && FoldIndex.Defined(),
            "No fold range begin, or feature set index, or baseline flag, or fold index in snapshot");
        const std::array<ui32, 4> progress = { foldRangeBegin, featureSetIdx, isTest, foldIdx };
        const std::array<ui32, 4> progressFromSnapshot = { *FoldRangeBegin, *FeatureSetIndex, *IsTest, *FoldIndex };
        return progress < progressFromSnapshot;
    }

    ui32 GetAbsoluteOffset() const {
        return EvalFeatureOptions.Offset;
    }

    TMaybe<ui32> FoldRangeBegin;
    TMaybe<ui32> FeatureSetIndex;
    TMaybe<bool> IsTest;
    TMaybe<ui32> FoldIndex;

private:
    THPTimer TrainTimer;
    ui32 IterationIdx = 0;
    const ui32 IterationCount;
    NCatboostOptions::TFeatureEvalOptions EvalFeatureOptions;
    TFeatureEvaluationSummary* const Summary;
    bool IsNextLoadValid = false;
};

static void EvaluateFeaturesImpl(
    const NCatboostOptions::TCatBoostOptions& catBoostOptions,
    const NCatboostOptions::TOutputFilesOptions& outputFileOptions,
    const NCatboostOptions::TFeatureEvalOptions& featureEvalOptions,
    const TMaybe<TCustomObjectiveDescriptor>& objectiveDescriptor,
    const TMaybe<TCustomMetricDescriptor>& evalMetricDescriptor,
    ui32 foldRangeBegin,
    const TCvDataPartitionParams& cvParams,
    TDataProviderPtr data,
    TFeatureEvaluationCallbacks* callbacks,
    TFeatureEvaluationSummary* results
) {
    const ui32 foldCount = cvParams.Initialized() ? cvParams.FoldCount : featureEvalOptions.FoldCount.Get();
    CB_ENSURE(data->ObjectsData->GetObjectCount() > foldCount, "Pool is too small to be split into folds");
    CB_ENSURE(data->ObjectsData->GetObjectCount() > featureEvalOptions.FoldSize.Get(), "Pool is too small to be split into folds");
    // TODO(akhropov): implement ordered split. MLTOOLS-2486.
    CB_ENSURE(
        data->ObjectsData->GetOrder() != EObjectsOrder::Ordered,
        "Feature evaluation for ordered objects data is not yet implemented"
    );

    const ui64 cpuUsedRamLimit
        = ParseMemorySizeDescription(catBoostOptions.SystemOptions->CpuUsedRamLimit.Get());

    TRestorableFastRng64 rand(catBoostOptions.RandomSeed);

    if (cvParams.Shuffle) {
        auto objectsGroupingSubset = NCB::Shuffle(data->ObjectsGrouping, 1, &rand);
        data = data->GetSubset(objectsGroupingSubset, cpuUsedRamLimit, &NPar::LocalExecutor());
    }

    TLabelConverter labelConverter;
    TMaybe<float> targetBorder = catBoostOptions.DataProcessingOptions->TargetBorder;
    NCatboostOptions::TCatBoostOptions dataSpecificOptions(catBoostOptions);
    TTrainingDataProviderPtr trainingData = GetTrainingData(
        std::move(data),
        /*isLearnData*/ true,
        TStringBuf(),
        Nothing(), // TODO(akhropov): allow loading borders and nanModes in CV?
        /*unloadCatFeaturePerfectHashFromRamIfPossible*/ true,
        /*ensureConsecutiveLearnFeaturesDataForCpu*/ false,
        outputFileOptions.AllowWriteFiles(),
        /*quantizedFeaturesInfo*/ nullptr,
        &dataSpecificOptions,
        &labelConverter,
        &targetBorder,
        &NPar::LocalExecutor(),
        &rand);

    CB_ENSURE(
        dynamic_cast<TQuantizedObjectsDataProvider*>(trainingData->ObjectsData.Get()),
        "Unable to quantize dataset (probably because it contains categorical features)"
    );

    UpdateYetiRankEvalMetric(trainingData->MetaInfo.TargetStats, Nothing(), &dataSpecificOptions);

    // If eval metric is not set, we assign it to objective metric
    InitializeEvalMetricIfNotSet(dataSpecificOptions.MetricOptions->ObjectiveMetric,
                                 &dataSpecificOptions.MetricOptions->EvalMetric);

    const auto overfittingDetectorOptions = dataSpecificOptions.BoostingOptions->OverfittingDetector;
    dataSpecificOptions.BoostingOptions->OverfittingDetector->OverfittingDetectorType = EOverfittingDetectorType::None;

    // internal training output shouldn't interfere with main stdout
    const auto loggingLevel = dataSpecificOptions.LoggingLevel;
    dataSpecificOptions.LoggingLevel = ELoggingLevel::Silent;

    const auto taskType = catBoostOptions.GetTaskType();
    THolder<IModelTrainer> modelTrainerHolder = TTrainerFactory::Construct(taskType);

    TSetLogging inThisScope(loggingLevel);

    TVector<TTrainingDataProviders> foldsData;
    TVector<TTrainingDataProviders> testFoldsData;
    constexpr bool isFixedMlTools3185 = false;
    PrepareFolds<TTrainingDataProviders>(
        trainingData,
        cvParams,
        featureEvalOptions,
        cpuUsedRamLimit,
        &foldsData,
        isFixedMlTools3185 ? &testFoldsData : nullptr,
        &NPar::LocalExecutor()
    );

    UpdatePermutationBlockSize(taskType, foldsData, &dataSpecificOptions);

    const ui32 approxDimension = GetApproxDimension(
        dataSpecificOptions,
        labelConverter,
        trainingData->TargetData->GetTargetDimension());
    const auto& metrics = CreateMetrics(
        dataSpecificOptions.MetricOptions,
        evalMetricDescriptor,
        approxDimension,
        trainingData->MetaInfo.HasWeights);
    CheckMetrics(metrics, dataSpecificOptions.LossFunctionDescription.Get().GetLossFunction());

    EMetricBestValue bestValueType;
    float bestPossibleValue;
    metrics.front()->GetBestValue(&bestValueType, &bestPossibleValue);

    if (!results->HasHeaderInfo()) {
        results->SetHeaderInfo(metrics, featureEvalOptions.FeaturesToEvaluate);
    }

    const ui32 offsetInRange = cvParams.Initialized() ? 0 : featureEvalOptions.Offset.Get();
    const auto trainFullModels = [&] (
        bool isTest,
        ui32 featureSetIdx,
        TVector<TTrainingDataProviders>* foldsData) {

        const auto topLevelTrainDir = outputFileOptions.GetTrainDir();
        const bool isCalcFstr = !outputFileOptions.CreateFstrIternalFullPath().empty();
        const bool isCalcRegularFstr = !outputFileOptions.CreateFstrRegularFullPath().empty();
        for (auto foldIdx : xrange(foldCount)) {
            const bool haveTrainResults = callbacks->HaveTrainResultsInSnapshot(
                foldRangeBegin,
                featureSetIdx,
                isTest,
                offsetInRange + foldIdx);
            if (haveTrainResults) {
                continue;
            }

            THPTimer timer;

            TFoldContext foldContext(
                foldRangeBegin + offsetInRange + foldIdx,
                taskType,
                outputFileOptions,
                std::move((*foldsData)[foldIdx]),
                rand.GenRand(),
                /*hasFullModel*/true);
            const auto foldDir = MakeFoldDirName(featureEvalOptions, isTest, featureSetIdx, foldContext.FoldIdx);
            callbacks->FoldRangeBegin = foldRangeBegin;
            callbacks->FeatureSetIndex = featureSetIdx;
            callbacks->IsTest = isTest;
            callbacks->FoldIndex = offsetInRange + foldIdx;
            callbacks->ResetIterationIndex();
            foldContext.OutputOptions.SetSaveSnapshotFlag(outputFileOptions.SaveSnapshot());
            Train(
                dataSpecificOptions,
                JoinFsPaths(topLevelTrainDir, foldDir),
                objectiveDescriptor,
                evalMetricDescriptor,
                labelConverter,
                metrics,
                /*isErrorTrackerActive*/false,
                callbacks,
                &foldContext,
                modelTrainerHolder.Get(),
                &NPar::LocalExecutor());

            if (testFoldsData) {
                CalcMetricsForTest(metrics, approxDimension, testFoldsData[foldIdx].Test[0], &foldContext);
            }

            results->AppendFeatureSetMetrics(isTest, featureSetIdx, foldContext.MetricValuesOnTest);

            CATBOOST_INFO_LOG << "Fold " << foldContext.FoldIdx << ": model built in " <<
                FloatToString(timer.Passed(), PREC_NDIGITS, 2) << " sec" << Endl;

            if (isCalcFstr || isCalcRegularFstr) {
                const auto& model = foldContext.FullModel.GetRef();
                const auto& floatFeatures = model.ModelTrees->GetFloatFeatures();
                const auto& catFeatures = model.ModelTrees->GetCatFeatures();
                const NCB::TFeaturesLayout layout(
                    TVector<TFloatFeature>(floatFeatures.begin(), floatFeatures.end()),
                    TVector<TCatFeature>(catFeatures.begin(), catFeatures.end())
                );
                const auto fstrType = outputFileOptions.GetFstrType();
                const auto effect = CalcFeatureEffect(model, /*dataset*/nullptr, fstrType, &NPar::LocalExecutor());
                results->FeatureStrengths[isTest][featureSetIdx].emplace_back(ExpandFeatureDescriptions(layout, effect));
                if (isCalcRegularFstr) {
                    const auto regularEffect = CalcRegularFeatureEffect(
                        effect,
                        model.GetNumCatFeatures(),
                        model.GetNumFloatFeatures());
                    results->RegularFeatureStrengths[isTest][featureSetIdx].emplace_back(
                        ExpandFeatureDescriptions(layout, regularEffect));
                }
            }

            (*foldsData)[foldIdx] = std::move(foldContext.TrainingData);
        }
    };

    if (featureEvalOptions.FeaturesToEvaluate->empty()) {
        trainFullModels(/*isTest*/false, /*featureSetIdx*/-1, &foldsData);
        return;
    }

    const auto useCommonBaseline = featureEvalOptions.FeatureEvalMode != NCB::EFeatureEvalMode::OneVsOthers;
    for (ui32 featureSetIdx : xrange(featureEvalOptions.FeaturesToEvaluate->size())) {
        const auto haveBaseline = featureSetIdx > 0 && useCommonBaseline;
        if (!haveBaseline) {
            auto newFoldsData = UpdateIgnoredFeaturesInLearn(
                taskType,
                featureEvalOptions,
                ETrainingKind::Baseline,
                featureSetIdx,
                foldsData);
            trainFullModels(/*isTest*/false, featureSetIdx, &newFoldsData);
        } else {
            results->BestMetrics[/*isTest*/0][featureSetIdx] = results->BestMetrics[/*isTest*/0][0];
            results->BestBaselineIterations[featureSetIdx] = results->BestBaselineIterations[0];
        }

        auto newFoldsData = UpdateIgnoredFeaturesInLearn(
            taskType,
            featureEvalOptions,
            ETrainingKind::Testing,
            featureSetIdx,
            foldsData);
        trainFullModels(/*isTest*/true, featureSetIdx, &newFoldsData);
    }
    if (outputFileOptions.AllowWriteFiles()) {
        for (auto isTest : {false, true}) {
            results->CreateLogs(
                outputFileOptions,
                featureEvalOptions,
                metrics,
                catBoostOptions.BoostingOptions->IterationCount,
                isTest,
                foldRangeBegin,
                callbacks->GetAbsoluteOffset());
        }
    }
}

static TString MakeAbsolutePath(const TString& path) {
    if (TFsPath(path).IsAbsolute()) {
        return path;
    }
    return JoinFsPaths(TFsPath::Cwd(), path);
}

TFeatureEvaluationSummary EvaluateFeatures(
    const NJson::TJsonValue& plainJsonParams,
    const NCatboostOptions::TFeatureEvalOptions& featureEvalOptions,
    const TMaybe<TCustomObjectiveDescriptor>& objectiveDescriptor,
    const TMaybe<TCustomMetricDescriptor>& evalMetricDescriptor,
    const TCvDataPartitionParams& cvParams,
    TDataProviderPtr data
) {
    const auto taskType = NCatboostOptions::GetTaskType(plainJsonParams);
    if (taskType == ETaskType::GPU) {
        CB_ENSURE(
            TTrainerFactory::Has(ETaskType::GPU),
            "Can't load GPU learning library. "
            "Module was not compiled or driver  is incompatible with package. "
            "Please install latest NVDIA driver and check again");
    }
    NCatboostOptions::TCatBoostOptions catBoostOptions(taskType);
    NCatboostOptions::TOutputFilesOptions outputFileOptions;
    LoadOptions(plainJsonParams, &catBoostOptions, &outputFileOptions);
    const auto& absoluteSnapshotPath = MakeAbsolutePath(outputFileOptions.GetSnapshotFilename());
    outputFileOptions.SetSnapshotFilename(absoluteSnapshotPath);

    const ui32 foldCount = cvParams.Initialized() ? cvParams.FoldCount : featureEvalOptions.FoldCount.Get();
    CB_ENSURE(foldCount > 0, "Fold count must be positive integer");

    const auto isObjectwise = IsObjectwiseEval(featureEvalOptions);
    const ui32 foldSize = featureEvalOptions.FoldSize;
    const auto& objectsGrouping = *data->ObjectsGrouping;
    const auto datasetSize = isObjectwise ? objectsGrouping.GetObjectCount() : objectsGrouping.GetGroupCount();
    const ui32 disjointFoldCount = CeilDiv(datasetSize, foldSize);
    const ui32 offset = featureEvalOptions.Offset;

    if (disjointFoldCount < offset + foldCount) {
        CB_ENSURE(
            cvParams.Shuffle,
            "Dataset contains too few objects or groups to evaluate features without shuffling. "
            "Please decrease fold size to at most " << datasetSize / (offset + foldCount) << ", or "
            "enable dataset shuffling in cross-validation "
            "(specify cv_no_suffle=False in Python or remove --cv-no-shuffle from command line).");
    }

    const auto foldRangeRandomSeeds = GenRandUI64Vector(CeilDiv(offset + foldCount, disjointFoldCount), catBoostOptions.RandomSeed);
    auto foldRangeRandomSeed = catBoostOptions;

    TFeatureEvaluationSummary summary;

    const auto callbacks = MakeHolder<TFeatureEvaluationCallbacks>(
        catBoostOptions.BoostingOptions->IterationCount,
        featureEvalOptions,
        &summary);

    if (outputFileOptions.SaveSnapshot() && NFs::Exists(absoluteSnapshotPath)) {
        callbacks->LoadSnapshot(taskType, absoluteSnapshotPath);
    }

    auto foldRangePart = featureEvalOptions;
    foldRangePart.Offset = offset % disjointFoldCount;
    foldRangePart.FoldCount = Min(disjointFoldCount - offset % disjointFoldCount, foldCount);
    ui32 foldRangeIdx = offset / disjointFoldCount;
    ui32 processedFoldCount = 0;
    while (processedFoldCount < foldCount) {
        foldRangeRandomSeed.RandomSeed = foldRangeRandomSeeds[foldRangeIdx];
        EvaluateFeaturesImpl(
            foldRangeRandomSeed,
            outputFileOptions,
            foldRangePart,
            objectiveDescriptor,
            evalMetricDescriptor,
            /*foldRangeBegin*/ foldRangeIdx * disjointFoldCount,
            cvParams,
            data,
            callbacks.Get(),
            &summary
        );
        ++foldRangeIdx;
        processedFoldCount += foldRangePart.FoldCount.Get();
        foldRangePart.Offset = 0;
        foldRangePart.FoldCount = Min(disjointFoldCount, foldCount - processedFoldCount);
    }
    summary.CalcWxTestAndAverageDelta();
    return summary;
}
