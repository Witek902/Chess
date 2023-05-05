#pragma once

#include "Position.hpp"
#include "MoveList.hpp"
#include "TranspositionTable.hpp"
#include "MoveOrderer.hpp"
#include "Time.hpp"
#include "Memory.hpp"
#include "Score.hpp"
#include "NeuralNetworkEvaluator.hpp"
#include "NodeCache.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <condition_variable>
#include <functional>

#ifndef CONFIGURATION_FINAL
#define COLLECT_SEARCH_STATS
#endif // CONFIGURATION_FINAL

// #define USE_EVAL_PROBING

struct SearchLimits
{
    // a time point where search started
    TimePoint startTimePoint = TimePoint::Invalid();

    // minimum time after which root singularity search kicks in
    TimePoint rootSingularityTime = TimePoint::Invalid();

    // suggested search time, it's checked every iteration so can be exceeded
    TimePoint idealTime = TimePoint::Invalid();

    // maximum allowed search time, after that all search must be stopped immediately
    TimePoint maxTime = TimePoint::Invalid();

    // maximum allowed searched nodes
    uint64_t maxNodes = UINT64_MAX;

    // maximum allowed base search depth (excluding quiescence, extensions, etc.)
    uint16_t maxDepth = UINT16_MAX;

    // enable mate search, disables all pruning
    bool mateSearch = false;

    // in analysis mode full PV lines are searched
    bool analysisMode = false;
};

#ifdef USE_EVAL_PROBING

// Utility that allows for collecting evaluated positions during the search
// This is used for collecting positions for parameter tuning
class EvalProbingInterface
{
public:
    virtual void ReportPosition(const Position& pos, ScoreType eval) = 0;
};

#endif // USE_EVAL_PROBING

struct SearchParam
{
    // shared transposition table
    TranspositionTable& transpositionTable;

    // search limits
    SearchLimits limits;

    uint32_t numThreads = 1;

    // number of PV lines to report
    uint32_t numPvLines = 1;

    // exclude this root moves from the search
    std::vector<Move> excludedMoves;

    // in pondering we don't care about limits
    std::atomic<bool> isPonder = false;

    // used to stop search
    std::atomic<bool> stopSearch = false;

    // print UCI-style output
    bool debugLog = true;

    // probe tablebases at the root
    bool useRootTablebase = true;

    bool useAspirationWindows = true;

    // use colors in console output to make it more readable
    bool colorConsoleOutput = false;

    // move notation for PV lines printing
    MoveNotation moveNotation = MoveNotation::LAN;

    // print verbose debug stats (not UCI compatible)
    bool verboseStats = false;

    // show win/draw/loss probabilities along with classic cp score
    bool showWDL = false;

#ifdef USE_EVAL_PROBING
    // optional eval probing interface
    EvalProbingInterface* evalProbingInterface = nullptr;
#endif // USE_EVAL_PROBING
};

struct PvLine
{
    std::vector<Move> moves;
    ScoreType score = InvalidValue;
    ScoreType tbScore = InvalidValue;
};

using SearchResult = std::vector<PvLine>;

struct NodeInfo
{
    Position position;

    NodeInfo* parentNode = nullptr;

    // ignore given moves in search, used for singular extensions
    PackedMove filteredMove = PackedMove::Invalid();

    uint8_t pvIndex = 0;

    uint8_t doubleExtensions = 0;

    // remaining depth
    int16_t depth = 0;

    // depth in ply (depth counting from root)
    uint16_t height = 0;

    ScoreType alpha;
    ScoreType beta;

    ScoreType staticEval = InvalidValue;

    Move previousMove = Move::Invalid();

    TPieceScore<int32_t> psqtScore = { INT32_MIN, INT32_MIN };

    bool isPvNodeFromPrevIteration = false;
    bool isCutNode = false;
    bool isNullMove = false;
    bool isInCheck = false;
    bool isSingularSearch = false;

    NNEvaluatorContext* nnContext = nullptr;

    uint16_t pvLength = 0;
    PackedMove pvLine[MaxSearchDepth];

    INLINE bool IsPV() const { return (beta - alpha) != 1; }
};

struct SearchThreadStats
{
    uint64_t nodesTemp = 0;     // flushed to global stats
    uint64_t nodesTotal = 0;
    uint64_t quiescenceNodes = 0;
    uint32_t maxDepth = 0;
    uint64_t tbHits = 0;

    void OnNodeEnter(uint32_t height)
    {
        nodesTemp++;
        nodesTotal++;
        maxDepth = std::max(maxDepth, height);
    }
};

struct SearchStats
{
    std::atomic<uint64_t> nodes = 0;
    std::atomic<uint64_t> quiescenceNodes = 0;
    std::atomic<uint32_t> maxDepth = 0;
    std::atomic<uint64_t> tbHits = 0;

#ifdef COLLECT_SEARCH_STATS
    static const int32_t EvalHistogramMaxValue = 1600;
    static const int32_t EvalHistogramBins = 100;
    uint64_t ttHits = 0;
    uint64_t ttWrites = 0;

    uint64_t numPvNodes = 0;
    uint64_t numCutNodes = 0;
    uint64_t numAllNodes = 0;

    uint64_t expectedCutNodesSuccess = 0;
    uint64_t expectedCutNodesFailure = 0;

    uint64_t totalBetaCutoffs = 0;
    uint64_t betaCutoffHistogram[MoveList::MaxMoves] = { 0 };
    uint64_t ttMoveBetaCutoffs[TTEntry::NumMoves] = { };
    uint64_t goodCaptureCutoffs = 0;
    uint64_t badCaptureCutoffs = 0;
    uint64_t killerMoveBetaCutoffs[MoveOrderer::NumKillerMoves] = { };
    uint64_t counterMoveCutoffs = 0;
    uint64_t quietCutoffs = 0;

    uint64_t evalHistogram[EvalHistogramBins] = { 0 };
#endif // COLLECT_SEARCH_STATS

    void Append(SearchThreadStats& threadStats, bool flush = false);

    SearchStats& operator = (const SearchStats& other)
    {
        nodes = other.nodes.load();
        quiescenceNodes = other.quiescenceNodes.load();
        maxDepth = other.maxDepth.load();
        tbHits = other.tbHits.load();
        return *this;
    }
};

class Search
{
public:

    Search();
    ~Search();

    void Clear();
    void StopWorkerThreads();

    void DoSearch(const Game& game, SearchParam& param, SearchResult& outResult, SearchStats* outStats = nullptr);

    const MoveOrderer& GetMoveOrderer() const;
    const NodeCache& GetNodeCache() const;

private:

    Search(const Search&) = delete;

    enum class BoundsType : uint8_t
    {
        Exact = 0,
        LowerBound = 1,
        UpperBound = 2,
    };

    struct SearchContext
    {
        const Game& game;
        SearchParam& searchParam;
        SearchStats& stats;
        std::vector<Move> excludedRootMoves;
    };

    struct AspirationWindowSearchParam
    {
        const Position& position;
        SearchParam& searchParam;
        uint32_t depth;
        uint8_t pvIndex;
        SearchContext& searchContext;
        ScoreType previousScore = 0;                  // score in previous ID iteration
        uint32_t threadID = 0;
    };

    struct ThreadData
    {
        std::atomic<bool> stopThread = false;
        std::thread thread;

        std::condition_variable taskFinishedCV;
        std::mutex taskFinishedMutex;
        bool taskFinished = false;

        std::condition_variable newTaskCV;
        std::mutex newTaskMutex;
        std::function<void()> callback;

        bool isMainThread = false;

        uint16_t rootDepth = 0;         // search depth at the root node in current iterative deepening step
        uint16_t depthCompleted = 0;    // recently completed search depth
        SearchResult pvLines;           // principal variation lines from recently completed search iteration
        SearchThreadStats stats;              // per-thread search stats
        uint32_t randomSeed;            // seed for random number generator

        // per-thread move orderer
        MoveOrderer moveOrderer;

        NodeCache nodeCache;

        // neural network context for each node height
        using NNEvaluatorContextPtr = std::unique_ptr<NNEvaluatorContext>;
        NNEvaluatorContextPtr nnContextStack[MaxSearchDepth];

        ThreadData();
        ThreadData(const ThreadData&) = delete;
        ThreadData(ThreadData&&) = delete;

        NNEvaluatorContext* GetNNEvaluatorContext(uint32_t height);

        // get PV move from previous depth iteration
        const Move GetPvMove(const NodeInfo& node) const;

        uint32_t GetRandomUint();
    };

    using ThreadDataPtr = std::unique_ptr<ThreadData>;

    std::vector<ThreadDataPtr> mThreadData;

    static constexpr uint32_t LMRTableSize = 64;
    uint8_t mMoveReductionTable[LMRTableSize][LMRTableSize];

    INLINE uint8_t GetDepthReduction(uint32_t depth, uint32_t moveIndex) const
    {
        return mMoveReductionTable[std::min(depth, LMRTableSize - 1)][std::min(moveIndex, LMRTableSize - 1)];
    }

    void BuildMoveReductionTable();

    static void WorkerThreadCallback(ThreadData* threadData);

    void ReportPV(const AspirationWindowSearchParam& param, const PvLine& pvLine, BoundsType boundsType, const TimePoint& searchTime) const;
    void ReportCurrentMove(const Move& move, int32_t depth, uint32_t moveNumber) const;

    void Search_Internal(const uint32_t threadID, const uint32_t numPvLines, const Game& game, SearchParam& param, SearchStats& outStats);
    PvLine AspirationWindowSearch(ThreadData& thread, const AspirationWindowSearchParam& param) const;
    ScoreType QuiescenceNegaMax(ThreadData& thread, NodeInfo& node, SearchContext& ctx) const;
    ScoreType NegaMax(ThreadData& thread, NodeInfo& node, SearchContext& ctx) const;

    // returns true if the search needs to be aborted immediately
    static bool CheckStopCondition(const ThreadData& thread, const SearchContext& ctx, bool isRootNode);
};