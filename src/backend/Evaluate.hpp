#pragma once

#include "Position.hpp"
#include "Move.hpp"
#include "Score.hpp"

#include <math.h>
#include <algorithm>

// TODO re-enable once new net is generated
// #define USE_ENDGAME_NEURAL_NETWORK

struct DirtyPiece;

extern const char* c_DefaultEvalFile;
extern const char* c_DefaultEndgameEvalFile;

// not using array of PieceScore, because Visual Studio compiler can't pack that nicely as data section of EXE,
// but generates ugly initialization code instead
using KingsPerspectivePSQT = int16_t[10][2 * Square::NumSquares];
extern const KingsPerspectivePSQT PSQT[Square::NumSquares / 2];

static constexpr PieceScore c_pawnValue     = {   97, 166 };
static constexpr PieceScore c_knightValue   = {  455, 371 };
static constexpr PieceScore c_bishopValue   = {  494, 385 };
static constexpr PieceScore c_rookValue     = {  607, 656 };
static constexpr PieceScore c_queenValue    = { 1427,1086 };
static constexpr PieceScore c_kingValue     = { std::numeric_limits<int16_t>::max(), std::numeric_limits<int16_t>::max() };

static constexpr PieceScore c_pieceValues[] =
{
    {0,0},
    c_pawnValue,
    c_knightValue,
    c_bishopValue,
    c_rookValue,
    c_queenValue,
    c_kingValue,
};

// if abs(simpleEval) > nnTresholdMax, then we don't use NN at all
// if abs(simpleEval) < nnTresholdMin, then we use NN purely
// between the two values, the NN eval and simple eval are blended smoothly
static constexpr int32_t c_nnTresholdMin = 768;
static constexpr int32_t c_nnTresholdMax = 1024;

bool TryLoadingDefaultEvalFile();
bool LoadMainNeuralNetwork(const char* path);

#ifdef USE_ENDGAME_NEURAL_NETWORK
bool TryLoadingDefaultEndgameEvalFile();
bool LoadEndgameNeuralNetwork(const char* path);
#endif // USE_ENDGAME_NEURAL_NETWORK

// scaling factor when converting from neural network output (logistic space) to centipawn value
// equal to 400/ln(10) = 173.7177...
static constexpr int32_t c_nnOutputToCentiPawns = 174;

// convert evaluation score (in pawns) to win probability
inline float EvalToWinProbability(float eval, uint32_t ply)
{
    // TODO better model
    const float a = 2.0f + ply / 240.0f;
    const float b = 0.5f;
    return 1.0f / (1.0f + expf((a - eval) / b));
}

// convert evaluation score (in pawns) to draw probability
inline float EvalToDrawProbability(float eval, uint32_t ply)
{
    const float winProb = EvalToWinProbability(eval, ply);
    const float lossProb = EvalToWinProbability(-eval, ply);
    return 1.0f - winProb - lossProb;
}

// convert evaluation score (in pawns) to expected game score
inline float EvalToExpectedGameScore(float eval)
{
    return 1.0f / (1.0f + powf(10.0, -eval / 4.0f));
}

// convert evaluation score (in centipawns) to expected game score
inline float InternalEvalToExpectedGameScore(int32_t eval)
{
    return EvalToExpectedGameScore(static_cast<float>(eval) * 0.01f);
}

// convert expected game score to evaluation score (in pawns)
inline float ExpectedGameScoreToEval(float score)
{
    ASSERT(score >= 0.0f && score <= 1.0f);
    score = std::clamp(score, 0.0f, 1.0f);
    return 4.0f * log10f(score / (1.0f - score));
}

// convert expected game score to evaluation score
inline ScoreType ExpectedGameScoreToInternalEval(float score)
{
    if (score > 0.99999f)
        return KnownWinValue - 1;
    else if (score < 0.00001f)
        return -KnownWinValue + 1;
    else
        return (ScoreType)std::clamp((int32_t)std::round(100.0f * ExpectedGameScoreToEval(score)),
            -KnownWinValue + 1, KnownWinValue - 1);
}

const TPieceScore<int32_t> ComputePSQT(const Position& pos);
void ComputeIncrementalPSQT(TPieceScore<int32_t>& score, const Position& pos, const DirtyPiece* dirtyPieces, uint32_t numDirtyPieces);

ScoreType Evaluate(const Position& position, NodeInfo* node = nullptr, bool useNN = true);
bool CheckInsufficientMaterial(const Position& position);
