#include "Common.hpp"
#include "ThreadPool.hpp"
#include "TrainerCommon.hpp"

#include "../backend/Position.hpp"
#include "../backend/PositionUtils.hpp"
#include "../backend/Game.hpp"
#include "../backend/Move.hpp"
#include "../backend/Search.hpp"
#include "../backend/TranspositionTable.hpp"
#include "../backend/Evaluate.hpp"
#include "../backend/Material.hpp"
#include "../backend/Endgame.hpp"
#include "../backend/Tablebase.hpp"
#include "../backend/PackedNeuralNetwork.hpp"
#include "../backend/Waitable.hpp"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <random>
#include <mutex>
#include <fstream>
#include <limits.h>

using namespace threadpool;

static const uint32_t cMaxIterations = 10000000;
static const uint32_t cNumTrainingVectorsPerIteration = 256 * 1024;
static const uint32_t cNumValidationVectorsPerIteration = 128 * 1024;
static const uint32_t cMinBatchSize = 256;
static const uint32_t cMaxBatchSize = 8 * 1024;
//static const uint32_t cNumNetworkInputs = 2 * 10 * 32 * 64;
static const uint32_t cNumNetworkInputs = 704;
static const uint32_t cNumVariants = 8;


static void PositionToSparseVector(const Position& pos, nn::TrainingVector& outVector)
{
    const uint32_t maxFeatures = 124;

    uint16_t features[maxFeatures];
    uint32_t numFeatures = pos.ToFeaturesVector(features, NetworkInputMapping::Full_Symmetrical);
    ASSERT(numFeatures <= maxFeatures);

    outVector.inputMode = nn::InputMode::SparseBinary;
    outVector.sparseBinaryInputs.clear();
    outVector.sparseBinaryInputs.reserve(numFeatures);

    for (uint32_t i = 0; i < numFeatures; ++i)
    {
        outVector.sparseBinaryInputs.push_back(features[i]);
    }
}

class NetworkTrainer
{
public:

    NetworkTrainer()
        : m_randomGenerator(m_randomDevice())
        , m_trainingLog("training.log")
    {
        m_trainingSet.resize(cNumTrainingVectorsPerIteration);
        m_validationPerThreadData.resize(ThreadPool::GetInstance().GetNumThreads());
    }

    void InitNetwork();

    void Train();

private:

    struct ValidationStats
    {
        float nnMinError = std::numeric_limits<float>::max();
        float nnMaxError = 0.0f, nnErrorSum = 0.0f;

        float nnPackedQuantizationErrorSum = 0.0f;
        float nnPackedMinError = std::numeric_limits<float>::max();
        float nnPackedMaxError = 0.0f, nnPackedErrorSum = 0.0f;

        float evalMinError = std::numeric_limits<float>::max();
        float evalMaxError = 0.0f, evalErrorSum = 0.0f;
    };

    struct alignas(CACHELINE_SIZE) ValidationPerThreadData
    {
        ValidationStats stats;
        nn::NeuralNetworkRunContext networkRunContext;
        uint8_t __padding[CACHELINE_SIZE];
    };

    nn::NeuralNetwork m_network;
    nn::NeuralNetworkRunContext m_runCtx;
    nn::NeuralNetworkTrainer m_trainer;
    nn::PackedNeuralNetwork m_packedNet;

    std::vector<PositionEntry> m_entries;
    std::vector<TrainingEntry> m_trainingSet;
    std::vector<ValidationPerThreadData> m_validationPerThreadData;

    alignas(CACHELINE_SIZE)
    std::atomic<uint64_t> m_numTrainingVectorsPassed = 0;

    alignas(CACHELINE_SIZE)
    std::mutex m_mutex;

    std::random_device m_randomDevice;
    std::mt19937 m_randomGenerator;

    std::ofstream m_trainingLog;

    void PrintPositionsStats();

    void GenerateTrainingSet(std::vector<TrainingEntry>& outEntries);

    void Validate(uint32_t iteration);
};

void NetworkTrainer::PrintPositionsStats()
{
    std::cout << "Training with " << m_entries.size() << " positions" << std::endl;

    uint64_t pieceCountStats[33];
    memset(pieceCountStats, 0, sizeof(pieceCountStats));
    for (const auto& entry : m_entries)
    {
        pieceCountStats[std::min(32u, entry.pos.occupied.Count())]++;
    }

    std::cout << "Piece count stats: " << std::endl;
    for (uint32_t i = 0; i <= 32; ++i)
    {
        std::cout
            << std::setw(2) << i << " "
            << std::setw(10) << pieceCountStats[i]
            << " (" << 100.0f * (float)pieceCountStats[i] / (float)m_entries.size() << "%)"
            << std::endl;
    }
}

void NetworkTrainer::InitNetwork()
{
    m_network.Init(cNumNetworkInputs,
                   { 1280, 1 },
                   nn::ActivationFunction::Sigmoid,
                   { 1, cNumVariants });

    //m_network.Init(cNumNetworkInputs,
    //               { 512, 16, 32, 1 },
    //               nn::ActivationFunction::Sigmoid,
    //               { 1, cNumVariants, cNumVariants, cNumVariants });

    //m_network.Load("eval-ckpt.nn");

    m_runCtx.Init(m_network);

    for (size_t i = 0; i < ThreadPool::GetInstance().GetNumThreads(); ++i)
    {
        m_validationPerThreadData[i].networkRunContext.Init(m_network);
    }
}

static uint32_t GetNetworkVariant(const Position& pos)
{
    return std::min((pos.GetNumPieces() - 2u) / 4u, 7u);
}

void NetworkTrainer::GenerateTrainingSet(std::vector<TrainingEntry>& outEntries)
{
    for (uint32_t i = 0; i < cNumTrainingVectorsPerIteration; ++i)
    {
        const uint64_t index = m_numTrainingVectorsPassed++;
        const PositionEntry& entry = m_entries[(uint32_t)(index % m_entries.size())];
        Position pos;
        VERIFY(UnpackPosition(entry.pos, pos));
        ASSERT(pos.IsValid());

        // flip the board randomly in pawnless positions
        if (pos.Whites().pawns == 0 && pos.Blacks().pawns == 0)
        {
            if (std::uniform_int_distribution<>(0, 1)(m_randomGenerator) != 0)
            {
                pos.MirrorVertically();
            }
        }

        PositionToSparseVector(pos, outEntries[i].trainingVector);
        outEntries[i].trainingVector.singleOutput = entry.score;
        outEntries[i].trainingVector.networkVariant = GetNetworkVariant(pos);
        //outEntries[i].trainingVector.lastLayerBias = (float)Evaluate(pos, nullptr, false) / (float)c_nnOutputToCentiPawns;
        outEntries[i].pos = pos;
    }
}

static void ParallelFor(const char* debugName, uint32_t arraySize, const threadpool::ParallelForTaskFunction& func, uint32_t maxThreads = 0)
{
    Waitable waitable;
    {
        TaskBuilder taskBuilder(waitable);
        taskBuilder.ParallelFor(debugName, arraySize, func, maxThreads);
    }
    waitable.Wait();
}

void NetworkTrainer::Validate(uint32_t iteration)
{
    // reset stats
    for (size_t i = 0; i < ThreadPool::GetInstance().GetNumThreads(); ++i)
    {
        m_validationPerThreadData[i].stats = ValidationStats();
    }

    Waitable waitable;
    {
        TaskBuilder taskBuilder(waitable);
        taskBuilder.ParallelFor("Validate", cNumValidationVectorsPerIteration, [this](const TaskContext& ctx, uint32_t i)
        {
            ValidationPerThreadData& threadData = m_validationPerThreadData[ctx.threadId];

            const float expectedValue = m_trainingSet[i].trainingVector.singleOutput;

            const ScoreType psqtValue = Evaluate(m_trainingSet[i].pos, nullptr, false);
            const ScoreType evalValue = Evaluate(m_trainingSet[i].pos);
            
            const std::vector<uint16_t>& features = m_trainingSet[i].trainingVector.sparseBinaryInputs;
            const uint32_t variant = m_trainingSet[i].trainingVector.networkVariant;
            const int32_t packedNetworkOutput = m_packedNet.Run(features.data(), (uint32_t)features.size(), variant);
            const float scaledPackedNetworkOutput = (float)packedNetworkOutput / (float)nn::OutputScale * c_nnOutputToCentiPawns / 100.0f;
            const float nnPackedValue = PawnToWinProbability(scaledPackedNetworkOutput /* + psqtValue / 100.0f*/);

            nn::NeuralNetwork::InputDesc inputDesc(features);
            inputDesc.variant = variant;
            //inputDesc.lastLayerBias = psqtValue / (float)c_nnOutputToCentiPawns;
            const nn::Values& networkOutput = m_network.Run(inputDesc, threadData.networkRunContext);
            const float nnValue = networkOutput[0];

            if (i + 1 == cNumValidationVectorsPerIteration)
            {
                std::cout
                    << m_trainingSet[i].pos.ToFEN() << std::endl << m_trainingSet[i].pos.Print() << std::endl
                    << "True Score:     " << expectedValue << " (" << WinProbabilityToCentiPawns(expectedValue) << ")" << std::endl
                    << "NN eval:        " << nnValue << " (" << WinProbabilityToCentiPawns(nnValue) << ")" << std::endl
                    << "Packed NN eval: " << nnPackedValue << " (" << WinProbabilityToCentiPawns(nnPackedValue) << ")" << std::endl
                    << "Static eval:    " << CentiPawnToWinProbability(evalValue) << " (" << evalValue << ")" << std::endl
                    << "PSQT eval:      " << CentiPawnToWinProbability(psqtValue) << " (" << psqtValue << ")" << std::endl
                    << std::endl;
            }

            ValidationStats& stats = threadData.stats;

            stats.nnPackedQuantizationErrorSum += (nnValue - nnPackedValue) * (nnValue - nnPackedValue);

            {
                const float error = expectedValue - nnValue;
                const float errorDiff = std::abs(error);
                stats.nnErrorSum += error * error;
                stats.nnMinError = std::min(stats.nnMinError, errorDiff);
                stats.nnMaxError = std::max(stats.nnMaxError, errorDiff);
            }

            {
                const float error = expectedValue - nnPackedValue;
                const float errorDiff = std::abs(error);
                stats.nnPackedErrorSum += error * error;
                stats.nnPackedMinError = std::min(stats.nnPackedMinError, errorDiff);
                stats.nnPackedMaxError = std::max(stats.nnPackedMaxError, errorDiff);
            }

            {
                const float error = expectedValue - CentiPawnToWinProbability(evalValue);
                const float errorDiff = std::abs(error);
                stats.evalErrorSum += error * error;
                stats.evalMinError = std::min(stats.evalMinError, errorDiff);
                stats.evalMaxError = std::max(stats.evalMaxError, errorDiff);
            }
        });
    }

    waitable.Wait();

    // accumulate stats
    ValidationStats stats;
    for (size_t i = 0; i < ThreadPool::GetInstance().GetNumThreads(); ++i)
    {
        const ValidationStats& threadStats = m_validationPerThreadData[i].stats;

        stats.nnPackedQuantizationErrorSum += threadStats.nnPackedQuantizationErrorSum;
        stats.nnErrorSum += threadStats.nnErrorSum;
        stats.nnMinError = std::min(stats.nnMinError, threadStats.nnMinError);
        stats.nnMaxError = std::max(stats.nnMaxError, threadStats.nnMaxError);
        stats.nnPackedErrorSum += threadStats.nnPackedErrorSum;
        stats.nnPackedMinError = std::min(stats.nnPackedMinError, threadStats.nnPackedMinError);
        stats.nnPackedMaxError = std::max(stats.nnPackedMaxError, threadStats.nnPackedMaxError);
        stats.evalErrorSum += threadStats.evalErrorSum;
        stats.evalMinError = std::min(stats.evalMinError, threadStats.evalMinError);
        stats.evalMaxError = std::max(stats.evalMaxError, threadStats.evalMaxError);
    }

    stats.nnErrorSum = sqrtf(stats.nnErrorSum / cNumValidationVectorsPerIteration);
    stats.nnPackedErrorSum = sqrtf(stats.nnPackedErrorSum / cNumValidationVectorsPerIteration);
    stats.evalErrorSum = sqrtf(stats.evalErrorSum / cNumValidationVectorsPerIteration);
    stats.nnPackedQuantizationErrorSum = sqrtf(stats.nnPackedQuantizationErrorSum / cNumValidationVectorsPerIteration);

    std::cout
        << "NN avg/min/max error:   " << std::setprecision(5) << stats.nnErrorSum << " " << std::setprecision(4) << stats.nnMinError << " " << std::setprecision(4) << stats.nnMaxError << std::endl
        << "PNN avg/min/max error:  " << std::setprecision(5) << stats.nnPackedErrorSum << " " << std::setprecision(4) << stats.nnPackedMinError << " " << std::setprecision(4) << stats.nnPackedMaxError << std::endl
        << "Quantization error:     " << std::setprecision(5) << stats.nnPackedQuantizationErrorSum << std::endl
        << "Eval avg/min/max error: " << std::setprecision(5) << stats.evalErrorSum << " " << std::setprecision(4) << stats.evalMinError << " " << std::setprecision(4) << stats.evalMaxError << std::endl;

    {
        const char* s_testPositions[] =
        {
            Position::InitPositionFEN,
            "rnbq1bnr/pppppppp/8/8/5k2/8/PPPPPPPP/RNBQKBNR w KQ - 0 1",         // black king in the center
            "r1bq1rk1/1pp2ppp/8/4pn2/B6b/1PN2P2/PBPP1P2/RQ2R1K1 b - - 1 12",
            "k7/ppp5/8/8/8/8/P7/K7 w - - 0 1",  // should be at least -200
            "7k/ppp5/8/8/8/8/P7/7K w - - 0 1",  // should be at least -200
            "7k/pp6/8/8/8/8/PP6/7K w - - 0 1",   // should be 0
            "k7/pp6/8/8/8/8/P7/K7 w - - 0 1",   // should be 0
            "r6k/7p/8/8/8/8/7P/1R5K w - - 0 1", // should be 0
        };

        for (const char* testPosition : s_testPositions)
        {
            Position pos(testPosition);
            nn::TrainingVector vec;
            PositionToSparseVector(pos, vec);

            // const ScoreType psqtValue = Evaluate(pos, nullptr, false);

            nn::NeuralNetwork::InputDesc inputDesc(vec.sparseBinaryInputs);
            inputDesc.variant = GetNetworkVariant(pos);
            //inputDesc.lastLayerBias = psqtValue / (float)c_nnOutputToCentiPawns;
            const float nnValue = m_network.Run(inputDesc, m_runCtx)[0];

            const int32_t packedNetworkOutput = m_packedNet.Run(vec.sparseBinaryInputs.data(), (uint32_t)vec.sparseBinaryInputs.size(), inputDesc.variant);
            const float scaledPackedNetworkOutput = (float)packedNetworkOutput / (float)nn::OutputScale * c_nnOutputToCentiPawns / 100.0f;
            const float nnPackedValue = PawnToWinProbability(scaledPackedNetworkOutput /* + psqtValue / 100.0f*/);

            std::cout << "TEST " << testPosition << "  " << WinProbabilityToCentiPawns(nnValue) << " " << WinProbabilityToCentiPawns(nnPackedValue) << std::endl;
        }
    }

    m_trainingLog << iteration << "\t" << stats.nnErrorSum << "\t" << stats.nnPackedErrorSum << std::endl;

    m_network.PrintStats();
}

void NetworkTrainer::Train()
{
    InitNetwork();

    LoadAllPositions(m_entries);

    // don't need tablebases after positions are loaded
    UnloadTablebase();

    PrintPositionsStats();

    std::vector<nn::TrainingVector> batch(cNumTrainingVectorsPerIteration);

    TimePoint prevIterationStartTime = TimePoint::GetCurrent();

    for (uint32_t iteration = 0; iteration < cMaxIterations; ++iteration)
    {
        if ((iteration % 1024) == 0)
        {
            std::cout << "Shuffling..." << std::endl;
            std::shuffle(m_entries.begin(), m_entries.end(), m_randomGenerator);
        }

        if (iteration == 0)
        {
            GenerateTrainingSet(m_trainingSet);
        }

        float learningRate = std::max(0.05f, 1.0f / (1.0f + 0.00002f * iteration));

        TimePoint iterationStartTime = TimePoint::GetCurrent();
        float iterationTime = (iterationStartTime - prevIterationStartTime).ToSeconds();
        prevIterationStartTime = iterationStartTime;
        
        // use validation set from previous iteration as training set in the current one
        ParallelFor("PrepareBatch", cNumTrainingVectorsPerIteration, [&batch, this](const TaskContext&, uint32_t i)
        {
            batch[i] = m_trainingSet[i].trainingVector;
        });

        // validation vectors generation can be done in parallel with training
        Waitable waitable;
        {
            TaskBuilder taskBuilder{ waitable };
            taskBuilder.Task("GenerateSet", [&](const TaskContext&)
            {
                GenerateTrainingSet(m_trainingSet);
            });

            taskBuilder.Task("Train", [this, iteration, &batch, &learningRate](const TaskContext& ctx)
            {
                nn::TrainParams params;
                params.batchSize = std::min(cMinBatchSize + iteration * cMinBatchSize, cMaxBatchSize);
                params.learningRate = learningRate;

                TaskBuilder taskBuilder{ ctx };
                m_trainer.Train(m_network, batch, params, &taskBuilder);
            });
        }
        waitable.Wait();

        m_network.ToPackedNetwork(m_packedNet);
        ASSERT(m_packedNet.IsValid());

        std::cout
            << "Epoch:                  " << iteration << std::endl
            << "Num training vectors:   " << m_numTrainingVectorsPassed << std::endl
            << "Learning rate:          " << learningRate << std::endl;

        Validate(iteration);

        std::cout << "Iteration time:   " << 1000.0f * iterationTime << " ms" << std::endl;
        std::cout << "Training rate :   " << ((float)cNumTrainingVectorsPerIteration / iterationTime) << " pos/sec" << std::endl << std::endl;

        if (iteration % 10 == 0)
        {
            const std::string name = "eval";
            m_network.Save((name + ".nn").c_str());
            m_packedNet.Save((name + ".pnn").c_str());
        }
    }
}


bool TrainNetwork()
{
    NetworkTrainer trainer;
    trainer.Train();

    return true;
}
