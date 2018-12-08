#include "feature_selector.h"

FeatureSelector::FeatureSelector(ros::NodeHandle nh, Estimator& estimator)
: nh_(nh), estimator_(estimator)
{

  // create future state horizon generator / manager
  hgen_ = std::unique_ptr<HorizonGenerator>(new HorizonGenerator(nh_));

  // set parameters for the horizon generator (extrinsics -- for visualization)
  hgen_->setParameters(estimator_.ric[0], estimator_.tic[0]);
}

// ----------------------------------------------------------------------------

void FeatureSelector::setParameters(double accVar, double accBiasVar)
{
  accVarDTime_ = accVar;
  accBiasVarDTime_ = accBiasVar;
}

// ----------------------------------------------------------------------------

void FeatureSelector::setNextStateFromImuPropagation(
    double imageTimestamp,
    const Eigen::Vector3d& P, const Eigen::Quaterniond& Q,
    const Eigen::Vector3d& V, const Eigen::Vector3d& a,
    const Eigen::Vector3d& w, const Eigen::Vector3d& Ba)
{
  //
  // State of previous frame (at the end of the fixed-lag window)
  //

  // TODO: Why is this not WINDOW_SIZE-1?
  state_k_.first.coeffRef(xTIMESTAMP) = state_k1_.first.coeff(xTIMESTAMP);
  state_k_.first.segment<3>(xPOS) = estimator_.Ps[WINDOW_SIZE];
  state_k_.first.segment<3>(xVEL) = estimator_.Vs[WINDOW_SIZE];
  state_k_.first.segment<3>(xB_A) = estimator_.Bas[WINDOW_SIZE];
  state_k_.second = estimator_.Rs[WINDOW_SIZE];
  
  //
  // (yet-to-be-corrected) state of current frame
  //

  // set the propagated-forward state of the current frame
  state_k1_.first.coeffRef(xTIMESTAMP) = imageTimestamp;
  state_k1_.first.segment<3>(xPOS) = P;
  state_k1_.first.segment<3>(xVEL) = V;
  state_k1_.first.segment<3>(xB_A) = Ba;
  state_k1_.second = Q;

  // the latest IMU measurement
  ak1_ = a;
  wk1_ = w;
}

// ----------------------------------------------------------------------------

void FeatureSelector::select(image_t& image, int kappa, 
        const std_msgs::Header& header, int nrImuMeasurements)
{
  //
  // Timing information
  //
  
  // frame time of previous image
  static double frameTime_k = header.stamp.toSec();

  // time difference between last frame and current frame
  double deltaF = header.stamp.toSec() - frameTime_k;

  // calculate the IMU sampling rate of the last frame-to-frame meas set
  double deltaImu = deltaF / nrImuMeasurements;


  //
  // Future State Generation
  //

  // We will need to know the state at each frame in the horizon, k:k+H.
  // Note that this includes the current optimized state, xk
  auto state_kkH = generateFutureHorizon(header, nrImuMeasurements, deltaImu, deltaF);

  if (visualize_) {
    hgen_->visualize(header, state_kkH);
  }


  //
  // Anticipation: Compute the Expected Information over the Horizon
  //

  // Calculate the information content from motion over the horizon (eq 15)
  auto OmegaIMU_kkH = calcInfoFromRobotMotion(state_kkH, nrImuMeasurements, deltaImu);

  // Add in prior information (eq 16)
  auto Omega_kkH = addOmegaPrior(OmegaIMU_kkH);

  // Calculate the information content of each of the new features
  auto Delta_ells = calcInfoFromFeatures(image);

  // Calculate the information content of each of the currently used features
  auto Delta_used_ells = calcInfoFromFeatures(image);


  //
  // Attention: Select a subset of features that maximizes expected information
  //

  // removes poor feature choices from image
  keepInformativeFeatures(image, kappa, Omega_kkH, Delta_ells, Delta_used_ells);


  // for next iteration
  frameTime_k = header.stamp.toSec();
}

// ----------------------------------------------------------------------------
// Private Methods
// ----------------------------------------------------------------------------

state_horizon_t FeatureSelector::generateFutureHorizon(
                                        const std_msgs::Header& header,
                                        int nrImuMeasurements,
                                        double deltaImu, double deltaFrame)
{

  // generate the horizon based on the requested scheme
  if (horizonGeneration_ == IMU) {
    return hgen_->imu(state_k_, state_k1_, ak1_, wk1_, nrImuMeasurements, deltaImu);
  } else { //if (horizonGeneration_ == GT) {
    return hgen_->groundTruth(state_k_, state_k1_, deltaFrame);
  }

}

// ----------------------------------------------------------------------------

std::vector<omega_horizon_t> FeatureSelector::calcInfoFromFeatures(const image_t& image)
{
  return {};
}

// ----------------------------------------------------------------------------

omega_horizon_t FeatureSelector::calcInfoFromRobotMotion(
        const state_horizon_t& state_kkH, double nrImuMeasurements, double deltaImu)
{
  // ** Build the large information matrix over the horizon (eq 15).
  // 
  // There is a sparse structure to the information matrix that we can exploit.
  // We can calculate the horizon info. matrix in blocks. Notice that each
  // pair of consecutive frames in the horizon create four 9x9 sub-blocks.
  // For example, for a horizon of H=3, the first pair of images (h=1) creates
  // a large information matrix like the following (each block is 9x9):
  // 
  //         |------------------------------------
  //         | At*Ω*A |  At*Ω  |    0   |    0   |
  //         |------------------------------------
  //         |   Ω*A  |    Ω   |    0   |    0   |
  //         |------------------------------------
  //         |    0   |    0   |    0   |    0   |
  //         |------------------------------------
  //         |    0   |    0   |    0   |    0   |
  //         |------------------------------------
  //    
  // The four non-zero sub-blocks shift along the diagonal as we loop through
  // the horizon (i.e., for h=2 there are zeros on all the edges and for h=3
  // the Ω is in the bottom-right corner). Note that the Ai matrix must be
  // recomputed for each h. The Ω matrix is calculated as the inverse of
  // the covariance in equation (52) and characterizes the noise in a
  // preintegrated set of IMU measurements using the linear IMU model.

  // NOTE: We are even more clever and only calculate the upper-triangular
  // and then transpose since this is a symmetric PSD matrix

  omega_horizon_t Omega_kkH = omega_horizon_t::Zero();

  for (int h=1; h<=HORIZON; ++h) { // for consecutive frames in horizon

    // convenience: frames (i, j) are a consecutive pair in horizon    
    const auto& Qi = state_kkH[h-1].second;
    const auto& Qj = state_kkH[h].second;

    // Create Ablk and Ω as explained in the appendix
    auto mats = createLinearImuMatrices(Qi, Qj, nrImuMeasurements, deltaImu);

    // convenience: select sub-blocks to add to, based on h
    Eigen::Ref<Eigen::MatrixXd> block1 = Omega_kkH.block<STATE_SIZE, STATE_SIZE>((h-1)*STATE_SIZE, (h-1)*STATE_SIZE);
    Eigen::Ref<Eigen::MatrixXd> block2 = Omega_kkH.block<STATE_SIZE, STATE_SIZE>((h-1)*STATE_SIZE, h*STATE_SIZE);
    Eigen::Ref<Eigen::MatrixXd> block3 = Omega_kkH.block<STATE_SIZE, STATE_SIZE>(h*STATE_SIZE, (h-1)*STATE_SIZE);
    Eigen::Ref<Eigen::MatrixXd> block4 = Omega_kkH.block<STATE_SIZE, STATE_SIZE>(h*STATE_SIZE, h*STATE_SIZE);

    // At*Ω*A (top-left sub-block)
    block1 += mats.second.transpose()*mats.first*mats.second;

    // At*Ω (top-right sub-block)
    auto tmp = mats.second.transpose()*mats.first;
    block2 += tmp;
    
    // Ω*A (bottom-left sub-block)
    block3 += tmp.transpose();

    // Ω (bottom-right sub-block)
    block4 += mats.second;
  }

  return Omega_kkH;
}

// ----------------------------------------------------------------------------

std::pair<omega_t, ablk_t> FeatureSelector::createLinearImuMatrices(
      const Eigen::Quaterniond& Qi, const Eigen::Quaterniond& Qj,
      double nrImuMeasurements, double deltaImu)
{
  //
  // "Pre-integrate" future IMU measurements over horizon
  //

  // helper matrices, equations (47) and (48)
  Eigen::Matrix3d Nij = Eigen::Matrix3d::Zero();
  Eigen::Matrix3d Mij = Eigen::Matrix3d::Zero();

  // initialize block coefficients
  double CCt_11 = 0;
  double CCt_12 = 0;

  // This is an IMU-rate for loop
  for (int i=0; i<nrImuMeasurements; ++i) {

    // slerp from Qi toward Qj by where we are in between the frames
    // (this will never slerp all the way to Qj)
    auto q = Qi.slerp(i/static_cast<double>(nrImuMeasurements), Qj);

    // so many indices...
    double jkh = (nrImuMeasurements - i - 0.5);
    Nij += jkh * q.toRotationMatrix();
    Mij += q.toRotationMatrix();

    // entries of CCt
    CCt_11 += jkh*jkh;
    CCt_12 += jkh;
  }

  // powers of IMU sampling period
  const double deltaImu_2 = deltaImu*deltaImu;
  const double deltaImu_3 = deltaImu_2*deltaImu;
  const double deltaImu_4 = deltaImu_3*deltaImu;

  //
  // Build cov(eta^imu_ij) -- see equation (52)
  // 

  // NOTE: In paper, bottom right entry of CCt should have (j-k), not (j-k-1).
  omega_t covImu = omega_t::Zero();
  covImu.block<3, 3>(0, 0) = Eigen::Matrix3d::Identity()
          * nrImuMeasurements * CCt_11 * deltaImu_4 * accVarDTime_;
  covImu.block<3, 3>(0, 3) = Eigen::Matrix3d::Identity()
          * CCt_11 * deltaImu_3 * accVarDTime_;
  covImu.block<3, 3>(3, 0) = covImu.block<3, 3>(0, 3).transpose();
  covImu.block<3, 3>(3, 3) = Eigen::Matrix3d::Identity()
          * nrImuMeasurements * deltaImu_2 * accVarDTime_;
  covImu.block<3, 3>(6, 6) = Eigen::Matrix3d::Identity()
          * nrImuMeasurements * accBiasVarDTime_;

  //
  // Build Ablk -- see equation (50)
  //
  
  Nij *= deltaImu_2;
  Mij *= deltaImu;

  ablk_t Ablk = -ablk_t::Identity();
  Ablk.block<3, 3>(0, 3) = -Eigen::Matrix3d::Identity() * nrImuMeasurements*deltaImu;
  Ablk.block<3, 3>(0, 6) = Nij;
  Ablk.block<3, 3>(3, 6) = Mij;

  return std::make_pair(covImu.inverse(), Ablk);
}

// ----------------------------------------------------------------------------

omega_horizon_t FeatureSelector::addOmegaPrior(const omega_horizon_t& OmegaIMU)
{
  return OmegaIMU;
}

// ----------------------------------------------------------------------------

void FeatureSelector::keepInformativeFeatures(image_t& image, int kappa,
          const omega_horizon_t& Omega_kkH,
          const std::vector<omega_horizon_t>& Delta_ells,
          const std::vector<omega_horizon_t>& Delta_used_ells)
{
  // Combine motion information with information from features that are already
  // being used in the VINS-Mono optimization backend
  omega_horizon_t Omega = Omega_kkH;
  for (const auto& Delta : Delta_used_ells) {
    Omega += Delta;
  }

  // subset of most informative features
  image_t subset;

  // select the indices of the best features
  for (int i=0; i<kappa; ++i) {

    // compute upper bounds
    auto U = sortedUpperBounds(subset, Omega, Delta_ells);

  }

  // return the subset
  subset.swap(image);
}

// ----------------------------------------------------------------------------

std::vector<std::pair<int,double>> FeatureSelector::sortedUpperBounds(
    const image_t& subset, const omega_horizon_t& Omega,
    const std::vector<omega_horizon_t>& Delta_ells)
{
  return {};
}

// ----------------------------------------------------------------------------

double FeatureSelector::logDet(const omega_horizon_t& Omega,
                               const omega_horizon_t& Delta_ell)
{
  return Utility::logdet(Omega + Delta_ell, true);
}

// ----------------------------------------------------------------------------

double FeatureSelector::logDetUB(const omega_horizon_t& Omega,
                                 const omega_horizon_t& Delta_ell)
{
  return 0;
}

// ----------------------------------------------------------------------------

double FeatureSelector::minEig(const omega_horizon_t& Omega,
                               const omega_horizon_t& Delta_ell)
{
  return 0;
}

// ----------------------------------------------------------------------------

double FeatureSelector::minEigUB(const omega_horizon_t& Omega,
                                 const omega_horizon_t& Delta_ell)
{
  return 0;
}

// ----------------------------------------------------------------------------
