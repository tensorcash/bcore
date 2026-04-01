#ifndef BITCOIN_VERIFICATION_QUICK_VERIFIER_H
#define BITCOIN_VERIFICATION_QUICK_VERIFIER_H

#include <primitives/proofblob.h>
#include <crypto/sha256.h>
#include <uint256.h>

#include <vector>
#include <string>
#include <memory>
#include <cmath>
#include <cstdint>

// Constants from shared-utils/config/constants.py
constexpr size_t POW_WINDOW_SIZE = 256;  // Must match Python's POW_WINDOW_SIZE
constexpr int TOPK_MIN = 5;
constexpr int TOPK_MAX = 50;
constexpr float TOPP_MIN = 0.1f;
constexpr float TOPP_MAX = 1.0f;
constexpr float TEMP_MIN = 0.25f;
constexpr float TEMP_MAX = 2.0f;
constexpr float ATOL = 0.0001f;  // From constants.py
constexpr float U_TOLERANCE = 1e-7f;  // Tighter tolerance for U values
constexpr uint32_t REUSE_SCORE_Q32_BITS = 32;
constexpr uint64_t REUSE_SCORE_Q32_ONE = 1ULL << REUSE_SCORE_Q32_BITS;
constexpr uint64_t REUSE_SCORE_CAP_Q32 = 247891519322ULL;  // ceil(57.716742 * 2^32)
// Proof versions below this are legacy and grandfathered (NOT reuse-entropy gated).
// Versions at/above this carry the q32 reuse-entropy gate. All chain history is v1,
// so legacy blocks can never be rejected by this gate. The activation height only
// governs when v1 is no longer *accepted* (min-version), not which rule applies.
constexpr uint8_t REUSE_GATE_VERSION = 2;

// Verification result enum matching Python ResponseValue
enum class VerificationResult {
    Quick_OK,
    Quick_Fail,
    Quick_OK_Smell_OK,
    Quick_OK_Smell_Fail,
    Quick_Fail_Smell_Fail,
    Quick_Fail_Model_Unregistered  // Model not found in registry
};

class QuickVerifier {
public:
    QuickVerifier() = default;
    ~QuickVerifier() = default;

    // Main verification entry point. Stateless: the reuse-entropy gate is enforced
    // iff proof.version >= REUSE_GATE_VERSION. No chain height needed.
    VerificationResult QuickVerify(const CProofBlob& proof);
    VerificationResult QuickVerify(const CProofBlob& proof, bool enforce_reuse_entropy);

    // Version-keyed reuse-entropy gate. Passes (returns true) for legacy proofs
    // below REUSE_GATE_VERSION; runs the q32 reuse-score gate for v2+ proofs.
    bool VerifyReuseEntropy(const CProofBlob& proof);

    // Get detailed error message if verification failed
    std::string GetLastError() const { return m_lastError; }

    // Enable intermediate value capture for testing
    void EnableIntermediateCapture(bool enable) { m_captureIntermediates = enable; }

    // Get captured intermediate values for testing
    struct IntermediateValues {
        std::vector<float> u_values;
        std::vector<std::vector<uint32_t>> dedupe_indices;
        std::vector<std::vector<float>> dedupe_values;
        std::vector<float> lower_bounds;
        std::vector<float> upper_bounds;
    };

    const IntermediateValues& GetIntermediates() const { return m_intermediates; }

private:
    // Core verification functions
    bool VerifyBlockSanity(const CProofBlob& proof);
    bool VerifyParameters(const CProofBlob& proof);
    bool VerifySequenceLightVectorized(const CProofBlob& proof, bool enforce_reuse_entropy);
    bool VerifyVDF(const CProofBlob& proof);
    bool VerifyEntropy(const std::vector<float>& lower_bounds,
                       const std::vector<float>& upper_bounds);
    bool VerifyFinalHash(const CProofBlob& proof);
    bool VerifyHeaderPoW(const CProofBlob& proof);
    bool VerifyModelRegistration(const CProofBlob& proof);

    // Helper functions for sequence verification
    std::vector<float> GetUValues(const CProofBlob& proof);
    float ComputeUValue(const std::vector<uint32_t>& context,
                        uint32_t step,
                        const CProofBlob& proof);

    // Deduplication: keep max logit per unique token
    void DedupeKeepMax(
        const std::vector<float>& logits,
        const std::vector<uint32_t>& indices,
        std::vector<float>& dedupe_logits,
        std::vector<uint32_t>& dedupe_indices
    );

    // Sampling verification for a single step
    bool VerifySampleStep(
        size_t step,
        const std::vector<float>& logits,
        const std::vector<uint32_t>& indices,
        const std::vector<uint32_t>& context,
        float u_value,
        uint32_t expected_token,
        const CProofBlob& proof,
        float& lower_out,
        float& upper_out
    );

public:
    // Apply sampling parameters (made public for testing)
    void ApplyTemperatureScaling(std::vector<float>& logits, float temperature);
    void ApplyRepetitionPenalty(std::vector<float>& logits,
                                const std::vector<uint32_t>& indices,
                                const std::vector<uint32_t>& context,
                                float repetition_penalty);
    void ApplyTopK(std::vector<float>& logits, uint32_t k);
    void ApplyTopP(std::vector<float>& logits,
                   const std::vector<uint32_t>& indices,
                   float p,
                   bool& borderline,
                   std::vector<float>& mask_p_h_values,
                   std::vector<float>& mask_p_l_values);

    // CDF computation (made public for verification_utils)
    void ComputeCDF(const std::vector<float>& logits,
                   const std::vector<uint32_t>& indices,
                   uint32_t expected_token,
                   float& lower,
                   float& upper,
                   int& position);

    // Utility functions (made public for verification_utils)
    double LogSumExp(const std::vector<float>& values);
    std::vector<float> ComputeProbabilities(const std::vector<float>& logits);
    std::vector<size_t> ArgSort(const std::vector<float>& values, bool descending = false);
    uint32_t ConservativeReuseBits(double mass_upper) const;

private:

    // SHA256-based U value generation helpers
    std::vector<uint8_t> TokensToBytes(const std::vector<uint32_t>& tokens);
    std::vector<uint8_t> Uint32ToLittleEndian(uint32_t value);
    std::vector<uint8_t> StringToBytes(const std::string& str);
    float DigestToU(const uint256& digest);

    // Member variables
    std::string m_lastError;
    bool m_captureIntermediates = false;
    IntermediateValues m_intermediates;

    // Cached values during verification
    size_t m_windowSize = 0;
    std::vector<uint32_t> m_promptTokens;
    std::vector<uint32_t> m_chosenTokens;
    std::vector<uint8_t> m_padMask;
};

#endif // BITCOIN_VERIFICATION_QUICK_VERIFIER_H
