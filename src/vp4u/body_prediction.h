// Low-latency pose resampling shared by the D4xx adapters.
#ifndef VP4U_BODY_PREDICTION_H
#define VP4U_BODY_PREDICTION_H

#include "vp4u_abi.h"

namespace vp4u {

// Extrapolate the latest measured body to the callback time. Times use the
// same monotonic clock and are expressed in milliseconds. The function always
// starts from `latest`, keeps untracked joints unchanged, bounds implausible
// joint velocity, and caps the total horizon.
void predict_body(ParsedBody* output,
                  const ParsedBody& previous,
                  const ParsedBody& latest,
                  double previous_sample_ms,
                  double latest_sample_ms,
                  double callback_time_ms,
                  float additional_prediction_ms,
                  float maximum_horizon_ms = 100.0f);

} // namespace vp4u

#endif // VP4U_BODY_PREDICTION_H
