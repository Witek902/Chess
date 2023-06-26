#pragma once

#include "Node.hpp"

namespace nn {

struct Gradients;

struct WeightsStorage
{
public:
    WeightsStorage(uint32_t inputSize, uint32_t outputSize, uint32_t numVariants);

    void Init();

    struct WeightsUpdateOptions
    {
        float learningRate = 1.0f;
        float gradientScale = 1.0f;
        float weightDecay = 0.0f;
        size_t iteration = 0;
    };

    void Update_Adadelta(const Gradients& gradients, const WeightsUpdateOptions& options);
    void Update_Adam(const Gradients& gradients, const WeightsUpdateOptions& options);

    uint32_t m_inputSize = 0;
    uint32_t m_outputSize = 0;
    bool m_isSparse = false;

    Values m_weightsMask;

    float m_weightsRange = 10.0f;
    float m_biasRange = 10.0f;

    struct Variant
    {
        Values m_weights;

        // used for learning
        Values m_gradientMoment1;
        Values m_gradientMoment2;
    };

    std::vector<Variant> m_variants;
};

using WeightsStoragePtr = std::shared_ptr<WeightsStorage>;

} // namespace nn