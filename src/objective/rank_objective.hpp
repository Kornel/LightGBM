/*!
 * Copyright (c) 2020 Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License. See LICENSE file in the project root for
 * license information.
 */
#ifndef LIGHTGBM_OBJECTIVE_RANK_OBJECTIVE_HPP_
#define LIGHTGBM_OBJECTIVE_RANK_OBJECTIVE_HPP_

#include <LightGBM/metric.h>
#include <LightGBM/objective_function.h>
#include <LightGBM/utils/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace LightGBM {

/*!
 * \brief Objective function for Ranking
 */
class RankingObjective : public ObjectiveFunction {
 public:
  explicit RankingObjective(const Config& config)
      : seed_(config.objective_seed) {}

  explicit RankingObjective(const std::vector<std::string>&) : seed_(0) {}

  ~RankingObjective() {}

  void Init(const Metadata& metadata, data_size_t num_data) override {
    num_data_ = num_data;
    // get label
    label_ = metadata.label();
    // get weights
    weights_ = metadata.weights();
    // get boundries
    query_boundaries_ = metadata.query_boundaries();
    if (query_boundaries_ == nullptr) {
      Log::Fatal("Ranking tasks require query information");
    }
    num_queries_ = metadata.num_queries();
  }

  void GetGradients(const double* score, score_t* gradients,
                    score_t* hessians) const override {
#pragma omp parallel for schedule(guided)
    for (data_size_t i = 0; i < num_queries_; ++i) {
      const data_size_t start = query_boundaries_[i];
      const data_size_t cnt = query_boundaries_[i + 1] - query_boundaries_[i];
      GetGradientsForOneQuery(i, cnt, label_ + start, score + start,
                              gradients + start, hessians + start);
      if (weights_ != nullptr) {
        for (data_size_t j = 0; j < cnt; ++j) {
          gradients[start + j] =
              static_cast<score_t>(gradients[start + j] * weights_[start + j]);
          hessians[start + j] =
              static_cast<score_t>(hessians[start + j] * weights_[start + j]);
        }
      }
    }
  }

  virtual void GetGradientsForOneQuery(data_size_t query_id, data_size_t cnt,
                                       const label_t* label,
                                       const double* score, score_t* lambdas,
                                       score_t* hessians) const = 0;

  const char* GetName() const override = 0;

  std::string ToString() const override {
    std::stringstream str_buf;
    str_buf << GetName();
    return str_buf.str();
  }

  bool NeedAccuratePrediction() const override { return false; }

 protected:
  int seed_;
  data_size_t num_queries_;
  /*! \brief Number of data */
  data_size_t num_data_;
  /*! \brief Pointer of label */
  const label_t* label_;
  /*! \brief Pointer of weights */
  const label_t* weights_;
  /*! \brief Query boundries */
  const data_size_t* query_boundaries_;
};

/*!
 * \brief Objective function for Lambdrank with NDCG
 */
class LambdarankNDCG : public RankingObjective {
 public:
  explicit LambdarankNDCG(const Config& config)
      : RankingObjective(config),
        sigmoid_(config.sigmoid),
        norm_(config.lambdarank_norm),
        truncation_level_(config.lambdarank_truncation_level),
        unbiased_(config.lambdarank_unbiased),
        eta_(config.lambdarank_eta) {
    label_gain_ = config.label_gain;
    // initialize DCG calculator
    DCGCalculator::DefaultLabelGain(&label_gain_);
    DCGCalculator::Init(label_gain_);
    sigmoid_table_.clear();
    inverse_max_dcgs_.clear();
    if (sigmoid_ <= 0.0) {
      Log::Fatal("Sigmoid param %f should be greater than zero", sigmoid_);
    }

    num_threads_ = omp_get_num_threads();
  }

  explicit LambdarankNDCG(const std::vector<std::string>& strs)
      : RankingObjective(strs) {}

  ~LambdarankNDCG() {}

  void Init(const Metadata& metadata, data_size_t num_data) override {
    RankingObjective::Init(metadata, num_data);
    DCGCalculator::CheckLabel(label_, num_data_);
    inverse_max_dcgs_.resize(num_queries_);
#pragma omp parallel for schedule(static)
    for (data_size_t i = 0; i < num_queries_; ++i) {
      inverse_max_dcgs_[i] = DCGCalculator::CalMaxDCGAtK(
          truncation_level_, label_ + query_boundaries_[i],
          query_boundaries_[i + 1] - query_boundaries_[i]);

      if (inverse_max_dcgs_[i] > 0.0) {
        inverse_max_dcgs_[i] = 1.0f / inverse_max_dcgs_[i];
      }
    }
    // construct sigmoid table to speed up sigmoid transform
    ConstructSigmoidTable();

    // initialize position bias vectors
    InitPositionBiasesAndGradients();
  }

  void GetGradients(const double* score, score_t* gradients,
                    score_t* hessians) const override {
    RankingObjective::GetGradients(score, gradients, hessians);

    if (unbiased_) {
      UpdatePositionBiasesAndGradients();
    }
  }

  inline void GetGradientsForOneQuery(data_size_t query_id, data_size_t cnt,
                                      const label_t* label, const double* score,
                                      score_t* lambdas,
                                      score_t* hessians) const override {
    //
    // query_id  : the query for which we are computing gradients
    // cnt       : number of documents returned for the query
    // label     : the Y values (relevance labels) for each document
    // score     : current predicted score for the associated document
    // lambdas   : array updated in place, gradients for this query
    // hessians  : array updated in place, second derivs for this query
    //

    // queries are processed in parallel
    // get id for current thread so safely accumulate bias corrections
    const int tid = omp_get_thread_num();  // get thread id

    // get max DCG on current query
    const double inverse_max_dcg = inverse_max_dcgs_[query_id];

    // initialize with zero
    for (data_size_t i = 0; i < cnt; ++i) {
      lambdas[i] = 0.0f;
      hessians[i] = 0.0f;
    }

    // get sorted indices for scores
    // by first fill the vector 0, 1, ... cnt-1
    std::vector<data_size_t> sorted_idx(cnt);
    for (data_size_t i = 0; i < cnt; ++i) {
      sorted_idx[i] = i;
    }

    // and then sort the result indices by score descending
    // eg [3, 2, 4, 1] means document 3 currently has highest score, document 1 lowest
    std::stable_sort(
        sorted_idx.begin(), sorted_idx.end(),
        [score](data_size_t a, data_size_t b) { return score[a] > score[b]; });

    // get best and worst score
    const double best_score = score[sorted_idx[0]];

    // worst score should be last item of sorted_idx
    // if that item is env min score (-inf), take the one before it
    data_size_t worst_idx = cnt - 1;
    if (worst_idx > 0 && score[sorted_idx[worst_idx]] == kMinScore) {
      worst_idx -= 1;
    }
    const double worst_score = score[sorted_idx[worst_idx]];

    // accumulator for lambdas used in normalization when norm_ = true
    double sum_lambdas = 0.0;

    // start accmulate lambdas by pairs that contain at least one document above truncation level
    // working across the cnt number of documents for the query
    // this going in order of score desc since start w sorted_idx[0]
    for (data_size_t i = 0; i < cnt - 1 && i < truncation_level_; ++i) {
      if (score[sorted_idx[i]] == kMinScore) { continue; }

      // compare doc i to all other docs j of differing level of relevance
      for (data_size_t j = i + 1; j < cnt; ++j) {
        if (score[sorted_idx[j]] == kMinScore) { continue; }

        // skip pairs with the same labels
        if (label[sorted_idx[i]] == label[sorted_idx[j]]) { continue; }

        data_size_t high_rank, low_rank;
        if (label[sorted_idx[i]] > label[sorted_idx[j]]) {
          high_rank = i;
          low_rank = j;
        } else {
          high_rank = j;
          low_rank = i;
        }

        // info of more relevant doc
        const data_size_t high = sorted_idx[high_rank];  // doc index in query results
        const int high_label = static_cast<int>(label[high]);  // label (Y)
        const double high_score = score[high];
        const double high_label_gain = label_gain_[high_label];  // default: 2^high_label - 1
        const double high_discount = DCGCalculator::GetDiscount(high_rank);  // 1/log2(2 + i)

        // info of less relevant doc
        const data_size_t low = sorted_idx[low_rank];
        const int low_label = static_cast<int>(label[low]);
        const double low_score = score[low];
        const double low_label_gain = label_gain_[low_label];
        const double low_discount = DCGCalculator::GetDiscount(low_rank);

        //
        // note on subsequent comments
        // in the papers, we assume i is more relevant than j
        // formula numbers are from unbiased lambdamart paper
        //
        // si - sj
        const double delta_score = high_score - low_score;

        // get dcg gap
        // default: 2^i - 2^j > 0
        const double dcg_gap = high_label_gain - low_label_gain;

        // get discount of this pair
        // |1/log2(2 + i) - 1/log2(2 + j)|
        const double paired_discount = fabs(high_discount - low_discount);

        // get delta NDCG
        // (2^i - 2^j) * |1/log2(2 + i) - 1/log2(2 + j)| / max_dcg
        double delta_pair_NDCG = dcg_gap * paired_discount * inverse_max_dcg;

        // regularize the delta_pair_NDCG by score distance
        if ((norm_ || unbiased_) && best_score != worst_score) {
          delta_pair_NDCG /= (0.01f + fabs(delta_score));
        }

        // calculate lambda for this pair
        // part of (34)
        // (34) and (36) are used to get the unbiased gradient estimates
        // in original this first p_lambda is double what it should be but ends up not mattering
        double p_lambda = GetSigmoid(delta_score);  // 1 / (1 + e^(sigmoid_ * (si - sj)))

        // d/dx {part of (34)} from above
        // ** confirmed wrong in original **
        // see subsequent p_hessian comments, but appears to be wrong
        // if sigmoid_ was meant to be 2 in the paper, that would be multiplied out front
        // it wouldn't be lambda * (2 - lambda) but instead 2 * lambda (1 - lambda)
        double p_hessian = p_lambda * (1.0f - p_lambda);

        if (unbiased_) {
          // formula (37)
          // used to get t+ and t- from (30)/(31) respectively
          // ** confirmed correct here and (accidentally) in original **
          // orig has log(2/(2 - p_lambda))
          //   let bad_exp = e^(2 * sigmoid_ * (si - sj))
          //   let bad_denom = 1 + bad_exp
          //
          //   2/(2-(2/bad_denom))
          //   2 / { (2*bad_denom - 2)/bad_denom }
          //   2*bad_denom / (2 * (bad_denom - 1))
          //   bad_denom / bad_exp
          //   {1 + bad_exp} / bad_exp
          //   1 + 1/bad_exp
          //   1 + e^{-2*sigmoid_ * (si - sj)}
          //     ... so i think w the weird swaps/hard coded 2s and sigmoid_ = 1 2/(2-lambda) is right
          //     ... which means 1/(1-lambda) is correct here
          double p_cost = log(1.0f / (1.0f - p_lambda)) * delta_pair_NDCG;

          // formula (30)
          // more relevant (clicked) gets debiased by less relevant (unclicked)
          i_costs_buffer_[tid][high_rank] += p_cost / j_biases_pow_[low_rank];

          // formula (31)
          // and vice versa
          j_costs_buffer_[tid][low_rank] += p_cost / i_biases_pow_[high_rank];
        }

        // update
        // ** confirmed p_lambda is correct **
        // rest of (34) with formula (36) for debiasing
        // orig doesn't have sigmoid_
        // if not unbiased_
        //    {1/(1 + e^(sigmoid_ * (si - sj)))} * -sigmoid_ * (2^i - 2^j) * |1/log2(2 + i) - 1/log2(2 + j)| * (1/max_dcg)
        // note that orig has
        //    {2/(1 + e^(2 * sigmoid_ * (si - sj)))} * -1 * (2^i - 2^j) * |1/log2(2 + i) - 1/log2(2 + j)| * (1/max_dcg)
        //    the 2 in the numerator and sigmoid_ missing from second term (delta_pair_NDCG) even out
        //    the 2 * sigmoid_ * (si - sj) in the exponent, however, makes no sense
        //    it appears the tests on that repo used an unset (default) sigmoid config value, which is 1
        //    this means that the paper's sigmoid_table_ denominator was computed correctly for sigmoid_ = 2
        //    as is described at (34) even though it was set for 1
        //    also means that leaving it out from p_lambda was (accidentally) fine
        p_lambda *= -sigmoid_ * delta_pair_NDCG / i_biases_pow_[high_rank] / j_biases_pow_[low_rank];

        // remainder of d/dx {(34) and (36) for debiasing}
        // ** confirmed wrong **
        // if not unbiased
        //    let good_exp = e^(sigmoid_ * (si - sj))
        //    let good_denom = 1 + good_exp
        //
        //    p_lambda * (1.0f - p_lambda) * sigmoid_ * sigmoid_ * delta_pair_ndcg
        //    {1/good_denom} * (1 - 1/good_denom) * sigmoid_ * sigmoid_ * delta_pair_ndcg
        //    {1/good_denom} * ((good_denom - 1)/good_denom) * sigmoid_ * sigmoid_ * delta_pair_ndcg
        //    sigmoid_ * sigmoid_ * good_exp * delta_pair_ndcg / {good_denom^2}
        //
        // orig has
        //    let bad_exp = e^(2 * sigmoid_ * (si - sj))
        //    let bad_denom = 1 + bad_exp
        //
        //    p_lambda * (2 - p_lambda) * 2 * delta_pair_ndcg
        //    {2/bad_denom} * (2 - (2/bad_denom)) * 2 * delta_pair_ndcg
        //    {2/bad_denom} * (2*(bad_denom - 1)/bad_denom) * 2 * delta_pair_ndcg
        //    2 * 2 * 2 * (bad_denom - 1) * delta_pair_ndcg / (bad_denom^2)
        //    2 * 2 * 2 * bad_exp * delta_pair_ndcg / (bad_denom^2)
        //
        //    if, as in the original ...
        //      * you WANT sigmoid_ = 2
        //      * but actually leave it as 1
        //      * and add 2s in as hardcoded constants
        //    then you end up w bad_denom == good_denom and
        //      2 * 2 * 2 * good_exp * delta_pair_ndcg / (good_denom^2)
        //      2 * sigmoid_ * sigmoid_ * good_exp * delta_pair_ndcg / {good_denom^2}
        //    and this has 1 too many 2s compared to what's here
        p_hessian *= sigmoid_ * sigmoid_ * delta_pair_NDCG / i_biases_pow_[high_rank] / j_biases_pow_[low_rank];

        lambdas[low] -= static_cast<score_t>(p_lambda);
        hessians[low] += static_cast<score_t>(p_hessian);
        lambdas[high] += static_cast<score_t>(p_lambda);
        hessians[high] += static_cast<score_t>(p_hessian);

        // lambda is negative, so use minus to accumulate
        sum_lambdas -= 2 * p_lambda;
      }
    }

    if (norm_ && sum_lambdas > 0) {
      double norm_factor = std::log2(1 + sum_lambdas) / sum_lambdas;
      for (data_size_t i = 0; i < cnt; ++i) {
        lambdas[i] = static_cast<score_t>(lambdas[i] * norm_factor);
        hessians[i] = static_cast<score_t>(hessians[i] * norm_factor);
      }
    }
  }

  inline double GetSigmoid(double score) const {
    if (score <= min_sigmoid_input_) {
      // too small, use lower bound
      return sigmoid_table_[0];
    } else if (score >= max_sigmoid_input_) {
      // too large, use upper bound
      return sigmoid_table_[_sigmoid_bins - 1];
    } else {
      return sigmoid_table_[static_cast<size_t>((score - min_sigmoid_input_) *
                                                sigmoid_table_idx_factor_)];
    }
  }

  void ConstructSigmoidTable() {
    // get boundary
    min_sigmoid_input_ = min_sigmoid_input_ / sigmoid_ / 2;
    max_sigmoid_input_ = -min_sigmoid_input_;
    sigmoid_table_.resize(_sigmoid_bins);
    // get score to bin factor
    sigmoid_table_idx_factor_ =
        _sigmoid_bins / (max_sigmoid_input_ - min_sigmoid_input_);
    // cache
    for (size_t i = 0; i < _sigmoid_bins; ++i) {
      const double score = i / sigmoid_table_idx_factor_ + min_sigmoid_input_;
      sigmoid_table_[i] = 1.0f / (1.0f + std::exp(score * sigmoid_));
    }
  }

  void InitPositionBiasesAndGradients() {
    i_biases_pow_.resize(truncation_level_);
    j_biases_pow_.resize(truncation_level_);
    i_costs_.resize(truncation_level_);
    j_costs_.resize(truncation_level_);

    for (int i = 0; i < truncation_level_; ++i) {
      // init position biases
      i_biases_pow_[i] = 1.0f;
      j_biases_pow_[i] = 1.0f;

      // init position gradients
      i_costs_[i] = 0.0f;
      j_costs_[i] = 0.0f;
    }

    // init gradient buffers for gathering results across threads
    for (int i = 0; i < num_threads_; i++) {
      i_costs_buffer_.emplace_back(truncation_level_, 0.0f);
      j_costs_buffer_.emplace_back(truncation_level_, 0.0f);
    }
  }

  void UpdatePositionBiasesAndGradients() const {
    // accumulate the parallel results
    for (int i = 0; i < num_threads_; i++) {
      for (int j = 0; j < truncation_level_; ++j) {
        i_costs_[j] += i_costs_buffer_[i][j];
        j_costs_[j] += j_costs_buffer_[i][j];

        // clear buffer for next run
        i_costs_buffer_[i][j] = 0.0f;
        j_costs_buffer_[i][j] = 0.0f;
      }
    }

    LogDebugPositionBiases();

    for (int i = 0; i < truncation_level_; ++i) {
      // Update bias
      i_biases_pow_[i] = pow(i_costs_[i] / i_costs_[0], eta_);
      j_biases_pow_[i] = pow(j_costs_[i] / j_costs_[0], eta_);
    }

    for (int i = 0; i < truncation_level_; ++i) {
      // Clear position info
      i_costs_[i] = 0.0f;
      j_costs_[i] = 0.0f;
    }
  }

  const char* GetName() const override { return "lambdarank"; }

 private:
  void LogDebugPositionBiases() const {
    std::stringstream message_stream;
    message_stream  << std::setw(10) << "position"
                    << std::setw(15) << "bias_i"
                    << std::setw(15) << "bias_j"
                    << std::setw(15) << "i_cost"
                    << std::setw(15) << "j_cost";
    Log::Debug(message_stream.str().c_str());

    for (int i = 0; i < truncation_level_; ++i) {
      message_stream  << std::setw(10) << i
                      << std::setw(15) << i_biases_pow_[i]
                      << std::setw(15) << j_biases_pow_[i]
                      << std::setw(15) << i_costs_[i]
                      << std::setw(15) << j_costs_[i]
                      << std::endl;
      Log::Debug(message_stream.str().c_str());
    }
  }

  /*! \brief Simgoid param */
  double sigmoid_;
  /*! \brief Normalize the lambdas or not */
  bool norm_;
  /*! \brief Truncation position for max DCG */
  int truncation_level_;
  /*! \brief Cache inverse max DCG, speed up calculation */
  std::vector<double> inverse_max_dcgs_;
  /*! \brief Cache result for sigmoid transform to speed up */
  std::vector<double> sigmoid_table_;
  /*! \brief Gains for labels */
  std::vector<double> label_gain_;
  /*! \brief Number of bins in simoid table */
  size_t _sigmoid_bins = 1024 * 1024;
  /*! \brief Minimal input of sigmoid table */
  double min_sigmoid_input_ = -50;
  /*! \brief Maximal input of sigmoid table */
  double max_sigmoid_input_ = 50;
  /*! \brief Factor that covert score to bin in sigmoid table */
  double sigmoid_table_idx_factor_;

  // bias correction variables
  /*! \brief power of (click) position biases */
  mutable std::vector<label_t> i_biases_pow_;

  /*! \brief power of (unclick) position biases */
  mutable std::vector<label_t> j_biases_pow_;

  // mutable double position cost;
  mutable std::vector<label_t> i_costs_;
  mutable std::vector<std::vector<label_t>> i_costs_buffer_;

  mutable std::vector<label_t> j_costs_;
  mutable std::vector<std::vector<label_t>> j_costs_buffer_;

  /*! \brief Should use unbiased lambdarank */
  bool unbiased_;
  /*! \brief Number of exponent */
  double eta_;

  /*! \brief Number of threads */
  int num_threads_;
};

/*!
 * \brief Implementation of the learning-to-rank objective function, XE_NDCG
 * [arxiv.org/abs/1911.09798].
 */
class RankXENDCG : public RankingObjective {
 public:
  explicit RankXENDCG(const Config& config) : RankingObjective(config) {}

  explicit RankXENDCG(const std::vector<std::string>& strs)
      : RankingObjective(strs) {}

  ~RankXENDCG() {}

  void Init(const Metadata& metadata, data_size_t num_data) override {
    RankingObjective::Init(metadata, num_data);
    for (data_size_t i = 0; i < num_queries_; ++i) {
      rands_.emplace_back(seed_ + i);
    }
  }

  inline void GetGradientsForOneQuery(data_size_t query_id, data_size_t cnt,
                                      const label_t* label, const double* score,
                                      score_t* lambdas,
                                      score_t* hessians) const override {
    // Skip groups with too few items.
    if (cnt <= 1) {
      for (data_size_t i = 0; i < cnt; ++i) {
        lambdas[i] = 0.0f;
        hessians[i] = 0.0f;
      }
      return;
    }

    // Turn scores into a probability distribution using Softmax.
    std::vector<double> rho(cnt, 0.0);
    Common::Softmax(score, rho.data(), cnt);

    // An auxiliary buffer of parameters used to form the ground-truth
    // distribution and compute the loss.
    std::vector<double> params(cnt);

    double inv_denominator = 0;
    for (data_size_t i = 0; i < cnt; ++i) {
      params[i] = Phi(label[i], rands_[query_id].NextFloat());
      inv_denominator += params[i];
    }
    // sum_labels will always be positive number
    inv_denominator = 1. / std::max<double>(kEpsilon, inv_denominator);

    // Approximate gradients and inverse Hessian.
    // First order terms.
    double sum_l1 = 0.0;
    for (data_size_t i = 0; i < cnt; ++i) {
      double term = -params[i] * inv_denominator + rho[i];
      lambdas[i] = static_cast<score_t>(term);
      // Params will now store terms needed to compute second-order terms.
      params[i] = term / (1. - rho[i]);
      sum_l1 += params[i];
    }
    // Second order terms.
    double sum_l2 = 0.0;
    for (data_size_t i = 0; i < cnt; ++i) {
      double term = rho[i] * (sum_l1 - params[i]);
      lambdas[i] += static_cast<score_t>(term);
      // Params will now store terms needed to compute third-order terms.
      params[i] = term / (1. - rho[i]);
      sum_l2 += params[i];
    }
    for (data_size_t i = 0; i < cnt; ++i) {
      lambdas[i] += static_cast<score_t>(rho[i] * (sum_l2 - params[i]));
      hessians[i] = static_cast<score_t>(rho[i] * (1.0 - rho[i]));
    }
  }

  double Phi(const label_t l, double g) const {
    return Common::Pow(2, static_cast<int>(l)) - g;
  }

  const char* GetName() const override { return "rank_xendcg"; }

 private:
  mutable std::vector<Random> rands_;
};

}  // namespace LightGBM
#endif  // LightGBM_OBJECTIVE_RANK_OBJECTIVE_HPP_
