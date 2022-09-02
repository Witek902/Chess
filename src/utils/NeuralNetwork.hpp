#include "Common.hpp"

#include <vector>
#include <deque>
#include <cmath>
#include <mutex>

namespace nn {

class Layer;
class NeuralNetwork;
class PackedNeuralNetwork;

using Values = std::vector<float, AlignmentAllocator<float, 32>>;

struct TrainingVector
{
    // intput as float values or active features list
    Values inputs;
    std::vector<uint16_t> features;

    Values output;
};

using TrainingSet = std::vector<TrainingVector>;

inline float InvTan(float x)
{
    return atanf(x);
}
inline float InvTanDerivative(float x)
{
    return 1.0f / (1.0f + x * x);
}

inline float Sigmoid(float x)
{
    return 1.0f / (1.0f + expf(-x));
}
inline float SigmoidDerivative(float x)
{
    float s = Sigmoid(x);
    return s * (1.0f - s);
}

inline float ClippedReLu(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x;
}
inline float ClippedReLuDerivative(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 0.0f;
    return 1.0f;
}
inline __m256 ClippedReLu(const __m256 x)
{
    return _mm256_min_ps(_mm256_set1_ps(1.0f), _mm256_max_ps(_mm256_setzero_ps(), x));
}
inline __m256 ClippedReLuDerivative(const __m256 x, const __m256 coeff)
{
    return _mm256_and_ps(coeff,
                         _mm256_and_ps(_mm256_cmp_ps(x, _mm256_setzero_ps(),  _CMP_GT_OQ),
                                       _mm256_cmp_ps(x, _mm256_set1_ps(1.0f), _CMP_LT_OQ)));
}

enum class ActivationFunction
{
    Linear,
    ClippedReLu,
    Sigmoid,
    ATan,
};

class LayerRunContext
{
public:
    std::vector<uint16_t> activeFeatures;
    Values input;
    bool useActiveFeaturesList{ false };

    Values linearValue;
    Values output;

    // used for learning
    Values inputGradient;

    void Init(const Layer& layer);
    void ComputeOutput(ActivationFunction activationFunction);
};

struct Gradients
{
    Values values;
    std::vector<bool> dirty;
};

class Layer
{
public:
    Layer(uint32_t inputSize, uint32_t outputSize);

    void InitWeights();
    void Run(const Values& in, LayerRunContext& ctx) const;
    void Run(const uint16_t* featureIndices, uint32_t numFeatures, LayerRunContext& ctx) const;
    void Backpropagate(const Values& error, LayerRunContext& ctx, Gradients& gradients) const;
    void UpdateWeights_SGD(float learningRate, const Gradients& gradients);
    void UpdateWeights_AdaDelta(float learningRate, const Gradients& gradients, const float gradientScale);
    void QuantizeWeights(float strength);

    uint32_t numInputs;
    uint32_t numOutputs;

    ActivationFunction activationFunction;

    Values weights;

    // used for learning
    Values gradientMean;
    Values gradientMoment;
};

class NeuralNetworkRunContext
{
public:
    std::vector<LayerRunContext> layers;
    
    // used for learning
    Values tempValues;

    void Init(const NeuralNetwork& network);
};

class NeuralNetwork
{
    friend class NeuralNetworkTrainer;

public:

    // Create multi-layer neural network
    void Init(uint32_t inputSize, const std::vector<uint32_t>& layersSizes, ActivationFunction outputLayerActivationFunc = ActivationFunction::Sigmoid);

    // save to file
    bool Save(const char* filePath) const;

    // load from file
    bool Load(const char* filePath);

    // convert to packed (quantized) network
    bool ToPackedNetwork(PackedNeuralNetwork& outNetwork) const;

    // Calculate neural network output based on arbitrary input
    const Values& Run(const Values& input, NeuralNetworkRunContext& ctx) const;

    // Calculate neural network output based on ssparse input (list of active features)
    const Values& Run(const uint16_t* featureIndices, uint32_t numFeatures, NeuralNetworkRunContext& ctx) const;

    void PrintStats() const;

    uint32_t GetLayersNumber() const
    {
        return (uint32_t)layers.size();
    }

    uint32_t GetInputSize() const
    {
        return layers.front().numInputs;
    }

    uint32_t GetOutputSize() const
    {
        return layers.back().numOutputs;
    }

    void ClampWeights();
    void ClampLayerWeights(size_t layerIndex, float weightRange, float biasRange, float weightQuantizationScale, float biasQuantizationScale);

    std::vector<Layer> layers;
};

class NeuralNetworkTrainer
{
public:
    void Train(NeuralNetwork& network, const TrainingSet& trainingSet, size_t batchSize, float learningRate = 0.5f, bool clampWeights = true);

    std::deque<Gradients> gradients;
    std::vector<NeuralNetworkRunContext> perThreadRunContext;
};

} // namespace nn
