#include <casadi/casadi.hpp>

#include "solver_ipopt.hpp"
#include "termcolor.hpp"
#include "bspline_utils.hpp"
#include "ros/ros.h"

#include <decomp_util/ellipsoid_decomp.h>  //For Polyhedron definition
#include <unsupported/Eigen/Splines>
#include <iostream>
#include <list>
#include <random>
#include <iostream>
#include <vector>
#include <fstream>

using namespace termcolor;

struct SolverIpopt::PImpl
{
  casadi::Function casadi_function_;
};

SolverIpopt::SolverIpopt(par_solver &par)
{
  deg_pol_ = par.deg_pol;
  num_pol_ = par.num_pol;

  p_ = deg_pol_;
  M_ = num_pol_ + 2 * p_;
  N_ = M_ - p_ - 1;
  num_of_segments_ = (M_ - 2 * p_);  // this is the same as num_pol_

  ///////////////////////////////////////

  a_star_bias_ = par.a_star_bias;

  basisConverter basis_converter;
  // basis used for collision
  if (par.basis == "MINVO")
  {
    basis_ = MINVO;
    M_pos_bs2basis_ = basis_converter.getMinvoPosConverters(num_of_segments_);
    M_vel_bs2basis_ = basis_converter.getMinvoVelConverters(num_of_segments_);
  }
  else if (par.basis == "BEZIER")
  {
    basis_ = BEZIER;
    M_pos_bs2basis_ = basis_converter.getBezierPosConverters(num_of_segments_);
    M_vel_bs2basis_ = basis_converter.getBezierVelConverters(num_of_segments_);
  }
  else if (par.basis == "B_SPLINE")
  {
    basis_ = B_SPLINE;
    M_pos_bs2basis_ = basis_converter.getBSplinePosConverters(num_of_segments_);
    M_vel_bs2basis_ = basis_converter.getBSplineVelConverters(num_of_segments_);
  }
  else
  {
    std::cout << red << "Basis " << par.basis << " not implemented yet" << reset << std::endl;
    std::cout << red << "============================================" << reset << std::endl;
    abort();
  }

  ///////////////////////////////////////
  ///////////////////////////////////////

  x_min_ = par.x_min;
  x_max_ = par.x_max;

  y_min_ = par.y_min;
  y_max_ = par.y_max;

  z_min_ = par.z_min;
  z_max_ = par.z_max;
  Ra_ = par.Ra;
  a_star_samp_x_ = par.a_star_samp_x;
  a_star_samp_y_ = par.a_star_samp_y;
  a_star_samp_z_ = par.a_star_samp_z;
  a_star_fraction_voxel_size_ = par.a_star_fraction_voxel_size;

  dc_ = par.dc;
  v_max_ = par.v_max;

  a_max_ = par.a_max;

  weight_ = par.weight;

  // separator_solver_ptr_ = new separator::Separator();
  // octopusSolver_ptr_ = new OctopusSearch(par.basis, num_pol_, deg_pol_, par.alpha_shrink);

  // // TODO: if C++14, use std::make_unique instead
  separator_solver_ptr_ = std::unique_ptr<separator::Separator>(new separator::Separator());
  octopusSolver_ptr_ =
      std::unique_ptr<OctopusSearch>(new OctopusSearch(par.basis, num_pol_, deg_pol_, par.alpha_shrink));

  // hack
  std::fstream myfile("/home/jtorde/Desktop/ws/src/mader/mader/matlab/index_instruction.txt", std::ios_base::in);
  myfile >> index_instruction_;
  std::cout << "index_instruction_= " << index_instruction_ << std::endl;
  /// end of hack

  m_casadi_ptr_ = std::unique_ptr<PImpl>(new PImpl());

  m_casadi_ptr_->casadi_function_ = casadi::Function::load("/home/jtorde/Desktop/ws/src/mader/mader/matlab/"
                                                           "my_function.casadi");
}

SolverIpopt::~SolverIpopt()
{
}

void SolverIpopt::getPlanes(std::vector<Hyperplane3D> &planes)
{
  planes = planes_;
}

int SolverIpopt::getNumOfLPsRun()
{
  return octopusSolver_ptr_->getNumOfLPsRun();
}

int SolverIpopt::getNumOfQCQPsRun()
{
  return num_of_QCQPs_run_;
}

void SolverIpopt::setMaxRuntimeKappaAndMu(double max_runtime, double kappa, double mu)
{
  kappa_ = kappa;
  mu_ = mu;
  max_runtime_ = max_runtime;
}

void SolverIpopt::setHulls(ConvexHullsOfCurves_Std &hulls)
{
  hulls_.clear();
  hulls_ = hulls;
  num_of_obst_ = hulls_.size();
  num_of_normals_ = num_of_segments_ * num_of_obst_;
}

//////////////////////////////////////////////////////////

// Note that t_final will be updated in case the saturation in deltaT_ has had effect
bool SolverIpopt::setInitStateFinalStateInitTFinalT(state initial_state, state final_state, double t_init,
                                                    double &t_final)
{
  ///////////////////////////
  Eigen::Vector3d p0 = initial_state.pos;
  Eigen::Vector3d v0 = initial_state.vel;
  Eigen::Vector3d a0 = initial_state.accel;

  Eigen::Vector3d pf = final_state.pos;
  Eigen::Vector3d vf = final_state.vel;
  Eigen::Vector3d af = final_state.accel;

  // here we saturate the value to ensure it is within the limits
  // the reason for this is the epsilon_tol_constraints (in the previous iteration, it may be slightly unfeasible)
  saturate(v0, -v_max_, v_max_);
  saturate(a0, -a_max_, a_max_);
  saturate(vf, -v_max_, v_max_);
  saturate(af, -a_max_, a_max_);

  initial_state_ = initial_state;
  final_state_ = final_state;

  std::cout << "initial_state= " << std::endl;
  initial_state.printHorizontal();

  std::cout << "final_state= " << std::endl;
  final_state.printHorizontal();

  //////////////////////////////

  deltaT_ = (t_final - t_init) / (1.0 * (M_ - 2 * p_ - 1 + 1));

  //////////////////////////////
  // Now make sure deltaT in knots_ is such that -v_max<=v1<=v_max is satisfied:
  // std::cout << bold << "deltaT_ before= " << deltaT_ << reset << std::endl;
  for (int axis = 0; axis < 3; axis++)
  {
    double upper_bound, lower_bound;
    if (fabs(a0(axis)) > 1e-7)
    {
      upper_bound = ((p_ - 1) * (sgn(a0(axis)) * v_max_(axis) - v0(axis)) / (a0(axis)));
      lower_bound = ((p_ - 1) * (-sgn(a0(axis)) * v_max_(axis) - v0(axis)) / (a0(axis)));

      // std::cout << "axis= " << axis << std::endl;
      // std::cout << "lower_bound= " << lower_bound << std::endl;
      // std::cout << "upper_bound= " << upper_bound << std::endl;

      ////////////////// Just for debugging
      if (upper_bound < lower_bound)
      {
        std::cout << red << bold << "This should never happen, aborting" << std::endl;
        abort();
      }
      //////////////////

      if (upper_bound <= 0)
      {
        std::cout << red << bold << "There is no way to satisfy v1" << std::endl;  //(deltat will be zero)
        return false;
      }

      saturate(deltaT_, std::max(0.0, lower_bound), upper_bound);
    }
    else
    {
      // do nothing: a0 ==0 for that axis, so that means that v1==v0, and therefore v1 satisfies constraints for that
      // axis
    }
  }

  // Eigen::Vector3d bound1 = ((p_ - 1) * (v_max_ - v0).array() / (a0.array()));
  // Eigen::Vector3d bound2 = ((p_ - 1) * (-v_max_ - v0).array() / (a0.array()));

  // // note that if any element of a0 is ==0.0, then its corresponding element in bound1 (or bound2) is +-infinity,
  // but
  // // valid  for the saturation below

  // saturate(deltaT_, std::min(bound1.x(), bound2.x()), std::max(bound1.x(), bound2.x()));
  // saturate(deltaT_, std::min(bound1.y(), bound2.y()), std::max(bound1.y(), bound2.y()));
  // saturate(deltaT_, std::min(bound1.z(), bound2.z()), std::max(bound1.z(), bound2.z()));

  // std::cout << "std::min(bound1.x(), bound2.x()= " << std::min(bound1.x(), bound2.x()) << std::endl;
  // std::cout << "std::max(bound1.x(), bound2.x()= " << std::max(bound1.x(), bound2.x()) << std::endl;

  // std::cout << "std::min(bound1.y(), bound2.y()= " << std::min(bound1.y(), bound2.y()) << std::endl;
  // std::cout << "std::max(bound1.y(), bound2.y()= " << std::max(bound1.y(), bound2.y()) << std::endl;

  // std::cout << "std::min(bound1.z(), bound2.z()= " << std::min(bound1.z(), bound2.z()) << std::endl;
  // std::cout << "std::max(bound1.z(), bound2.z()= " << std::max(bound1.z(), bound2.z()) << std::endl;

  // std::cout << bold << "deltaT_ after= " << deltaT_ << reset << std::endl;

  t_final = t_init + (1.0 * (M_ - 2 * p_ - 1 + 1)) * deltaT_;

  t_init_ = t_init;
  t_final_ = t_final;

  /////////////////////////

  /////////////////////////

  Eigen::RowVectorXd knots(M_ + 1);
  for (int i = 0; i <= p_; i++)
  {
    knots[i] = t_init_;
  }

  for (int i = (p_ + 1); i <= M_ - p_ - 1; i++)
  {
    knots[i] = knots[i - 1] + deltaT_;  // Assumming a uniform b-spline (internal knots are equally spaced)
  }

  for (int i = (M_ - p_); i <= M_; i++)
  {
    knots[i] = t_final_;
  }

  knots_ = knots;
  //////////////////

  //////////////////

  // See https://pages.mtu.edu/~shene/COURSES/cs3621/NOTES/spline/B-spline/bspline-derv.html
  // See also eq. 15 of the paper "Robust and Efficent quadrotor..."

  double t1 = knots_(1);
  double t2 = knots_(2);
  double tpP1 = knots_(p_ + 1);
  double t1PpP1 = knots_(1 + p_ + 1);

  double tN = knots_(N_);
  double tNm1 = knots_(N_ - 1);
  double tNPp = knots_(N_ + p_);
  double tNm1Pp = knots_(N_ - 1 + p_);

  // See Mathematica Notebook
  q0_ = p0;
  q1_ = p0 + (-t1 + tpP1) * v0 / p_;
  q2_ = (p_ * p_ * q1_ - (t1PpP1 - t2) * (a0 * (t2 - tpP1) + v0) - p_ * (q1_ + (-t1PpP1 + t2) * v0)) / ((-1 + p_) * p_);

  qN_ = pf;
  qNm1_ = pf + ((tN - tNPp) * vf) / p_;
  qNm2_ = (p_ * p_ * qNm1_ - (tNm1 - tNm1Pp) * (af * (-tN + tNm1Pp) + vf) - p_ * (qNm1_ + (-tNm1 + tNm1Pp) * vf)) /
          ((-1 + p_) * p_);

  return true;
}

bool SolverIpopt::optimize()
{
  std::cout << "in SolverIpopt::optimize" << std::endl;

  // reset some stuff
  traj_solution_.clear();
  // note that, for a v0 and a0 given, q2_ is not guaranteed to lie within the bounds. If that's the case --> keep
  // executing previous trajectory
  if (q2_.x() > x_max_ || q2_.x() < x_min_ ||  //////////////
      q2_.y() > y_max_ || q2_.y() < y_min_ ||  /////////////////
      q2_.z() > z_max_ || q2_.z() < z_min_)
  {
    std::cout << bold << red << "q2_ is not in [min, max]" << reset << std::endl;
    std::cout << "q2_= " << q2_.transpose() << std::endl;
    std::cout << "x_min_= " << x_min_ << ", x_max_=" << x_max_ << std::endl;
    std::cout << "y_min_= " << y_min_ << ", y_max_=" << y_max_ << std::endl;
    std::cout << "z_min_= " << z_min_ << ", z_max_=" << z_max_ << std::endl;
    return false;
  }
  bool guess_is_feasible = generateAStarGuess();  // I obtain q_quess_, n_guess_, d_guess_
  if (guess_is_feasible == false)
  {
    std::cout << "Planes haven't been found" << std::endl;
    return false;
  }
  n_ = n_guess_;
  d_ = d_guess_;

  ////////////////////////////////////
  ////////////////////////////////////
  //////////////////////////////////// CASADI!

  std::cout << "Casadi!" << std::endl;
  std::cout << "v_max_= " << v_max_.transpose() << std::endl;
  std::cout << "a_max_= " << a_max_.transpose() << std::endl;

  // Conversion DM <--> Eigen:  https://github.com/casadi/casadi/issues/2563

  auto eigen2std = [](Eigen::Vector3d &v) { return std::vector<double>{ v.x(), v.y(), v.z() }; };

  std::map<std::string, casadi::DM> map_arguments;
  map_arguments["theta_FOV_deg"] = 80.0;  // TODO: as a parameter
  map_arguments["p0"] = eigen2std(initial_state_.pos);
  map_arguments["v0"] = eigen2std(initial_state_.vel);
  map_arguments["a0"] = eigen2std(initial_state_.accel);
  map_arguments["pf"] = eigen2std(final_state_.pos);
  map_arguments["vf"] = eigen2std(final_state_.vel);
  map_arguments["af"] = eigen2std(final_state_.accel);
  map_arguments["y0"] = 0.0;
  map_arguments["ydot0"] = 0.0;
  map_arguments["v_max"] = eigen2std(v_max_);
  map_arguments["a_max"] = eigen2std(a_max_);
  map_arguments["total_time"] = (t_final_ - t_init_);
  map_arguments["all_w_fe"] = casadi::DM::ones(3, 15);
  map_arguments["c_jerk"] = 1.0;
  map_arguments["c_yaw"] = 0.0;
  map_arguments["c_vel_isInFOV"] = 0.0;
  map_arguments["c_final_pos"] = 0.2;

  std::cout << "Total time= " << (t_final_ - t_init_) << std::endl;

  int tmp_num_of_planes = 0;
  casadi::DM all_nd(casadi::Sparsity::dense(4, tmp_num_of_planes));  // TODO: do this just once
  for (int i = 0; i < tmp_num_of_planes; i++)
  {
    all_nd(0, i) = n_guess_[i].x();
    all_nd(1, i) = n_guess_[i].y();
    all_nd(2, i) = n_guess_[i].z();
    all_nd(3, i) = d_guess_[i];
  }

  map_arguments["all_nd"] = all_nd;  // casadi::DM::rand(4, 0);

  casadi::DM matrix_q_pos_guess(casadi::Sparsity::dense(3, (N_ + 1)));  // TODO: do this just once
  for (int i = 0; i < matrix_q_pos_guess.columns(); i++)
  {
    matrix_q_pos_guess(0, i) = q_guess_[i].x();
    matrix_q_pos_guess(1, i) = q_guess_[i].y();
    matrix_q_pos_guess(2, i) = q_guess_[i].z();
  }

  map_arguments["guess_CPs_Pos"] = matrix_q_pos_guess;

  std::cout << "GUESS\n" << map_arguments["guess_CPs_Pos"] << std::endl;

  casadi::DM matrix_q_yaw_guess(casadi::Sparsity::dense(1, 6));  // TODO: do this just once
  for (int i = 0; i < matrix_q_yaw_guess.columns(); i++)
  {
    matrix_q_yaw_guess(0, i) = rand();
  }

  map_arguments["guess_CPs_Yaw"] = matrix_q_yaw_guess;

  std::map<std::string, casadi::DM> result = m_casadi_ptr_->casadi_function_(map_arguments);

  /// Very hacky solution, see discussion at https://groups.google.com/g/casadi-users/c/1061E0eVAXM/m/dFHpw1CQBgAJ
  /// Inspired from https://gist.github.com/jgillis/9d12df1994b6fea08eddd0a3f0b0737f
  std::cout << "index_instruction_= " << index_instruction_ << std::endl;
  auto optimstatus =
      m_casadi_ptr_->casadi_function_.instruction_MX(index_instruction_).which_function().stats(1)["return_status"];
  /////////

  std::vector<Eigen::Vector3d> q;
  if (optimstatus == "Solve_Succeeded" || optimstatus == "Acceptable_Level")
  {
    std::cout << green << "IPOPT found a solution!" << reset << std::endl;

    auto all_pCPs = result["all_pCPs"];

    // copy the solution
    std::cout << "all_pCPs.columns()= " << all_pCPs.columns() << std::endl;
    for (int i = 0; i < all_pCPs.columns(); i++)
    {
      q.push_back(Eigen::Vector3d(double(all_pCPs(0, i)), double(all_pCPs(1, i)), double(all_pCPs(2, i))));
    }
    std::cout << "done!" << std::endl;
  }
  else
  {
    std::cout << red << "IPOPT failed to find a solution, using initial guess (which is feasible)" << reset
              << std::endl;
    q = q_guess_;
  }

  std::cout << "Calling CPs2TrajAndPwp!" << std::endl;

  // std::cout << "N_= " << N_ << std::endl;
  // std::cout << "p_= " << p_ << std::endl;
  // std::cout << "num_pol_= " << num_pol_ << std::endl;
  // std::cout << "knots_= " << knots_ << std::endl;
  // std::cout << "dc_= " << dc_ << std::endl;
  std::cout << "Control Points obtained= " << std::endl;
  for (auto q_i : q)
  {
    std::cout << q_i.transpose() << std::endl;
  }

  CPs2TrajAndPwp(q, traj_solution_, solution_, N_, p_, num_pol_, knots_, dc_);
  std::cout << "Called CPs2TrajAndPwp!" << std::endl;

  // Force last position =final_state_ (which it's not guaranteed because of the discretization with dc_)
  traj_solution_.back().vel = final_state_.vel;
  traj_solution_.back().accel = final_state_.accel;
  traj_solution_.back().jerk = Eigen::Vector3d::Zero();

  // std::cout << blue << "traj_solution_.size()=" << traj_solution_.size() <<reset<< std::endl;

  // Uncomment the following line if you wanna visualize the planes
  // fillPlanesFromNDQ(n_, d_, q);  // TODO: move this outside the SolverIpopt class

  return true;
}

void SolverIpopt::getSolution(PieceWisePol &solution)
{
  solution = solution_;
}

//////////////////////////////////////////
//////////////////////////////////////////
////////////////////////////////////////// THIS SHOULD GO TO A DIFFERENT PLACE
//////////////////////////////////////////
//////////////////////////////////////////
void SolverIpopt::fillPlanesFromNDQ(const std::vector<Eigen::Vector3d> &n, const std::vector<double> &d,
                                    const std::vector<Eigen::Vector3d> &q)
{
  planes_.clear();

  for (int obst_index = 0; obst_index < num_of_obst_; obst_index++)
  {
    for (int i = 0; i < num_of_segments_; i++)
    {
      int ip = obst_index * num_of_segments_ + i;  // index plane
      Eigen::Vector3d centroid_hull;
      findCentroidHull(hulls_[obst_index][i], centroid_hull);

      Eigen::Vector3d point_in_plane;

      Eigen::Matrix<double, 3, 4> Qmv, Qbs;  // minvo. each column contains a MINVO control point
      Qbs.col(0) = q[i];
      Qbs.col(1) = q[i + 1];
      Qbs.col(2) = q[i + 2];
      Qbs.col(3) = q[i + 3];

      transformPosBSpline2otherBasis(Qbs, Qmv, i);

      Eigen::Vector3d centroid_cps = Qmv.rowwise().mean();

      // the colors refer to the second figure of
      // https://github.com/mit-acl/separator/tree/06c0ddc6e2f11dbfc5b6083c2ea31b23fd4fa9d1

      // Equation of the blue planes is n'x+d == -1
      // Convert here to equation [A B C]'x+D ==0
      double A = n[ip].x();
      double B = n[ip].y();
      double C = n[ip].z();
      double D = d[ip] + 1;

      /////////////////// OPTION 1: point_in_plane = intersection between line  centroid_cps --> centroid_hull
      // bool intersects = getIntersectionWithPlane(centroid_cps, centroid_hull, Eigen::Vector4d(A, B, C, D),
      //                                            point_in_plane);  // result saved in point_in_plane

      //////////////////////////

      /////////////////// OPTION 2: point_in_plane = intersection between line  centroid_cps --> closest_vertex
      double dist_min = std::numeric_limits<double>::max();  // delta_min will contain the minimum distance between
                                                             // the centroid_cps and the vertexes of the obstacle
      int index_closest_vertex = 0;
      for (int j = 0; j < hulls_[obst_index][i].cols(); j++)
      {
        Eigen::Vector3d vertex = hulls_[obst_index][i].col(j);

        double distance_to_vertex = (centroid_cps - vertex).norm();
        if (distance_to_vertex < dist_min)
        {
          dist_min = distance_to_vertex;
          index_closest_vertex = j;
        }
      }

      Eigen::Vector3d closest_vertex = hulls_[obst_index][i].col(index_closest_vertex);

      bool intersects = getIntersectionWithPlane(centroid_cps, closest_vertex, Eigen::Vector4d(A, B, C, D),
                                                 point_in_plane);  // result saved in point_in_plane

      //////////////////////////

      if (intersects == false)
      {
        // TODO: this msg is printed sometimes in Multi-Agent simulations. Find out why
        std::cout << red << "There is no intersection, this should never happen (TODO)" << reset << std::endl;
        continue;  // abort();
      }

      Hyperplane3D plane(point_in_plane, n[i]);
      planes_.push_back(plane);
    }
  }
}

// returns 1 if there is an intersection between the segment P1-P2 and the plane given by coeff=[A B C D]
// (Ax+By+Cz+D==0)  returns 0 if there is no intersection.
// The intersection point is saved in "intersection"
bool SolverIpopt::getIntersectionWithPlane(const Eigen::Vector3d &P1, const Eigen::Vector3d &P2,
                                           const Eigen::Vector4d &coeff, Eigen::Vector3d &intersection)
{
  double A = coeff[0];
  double B = coeff[1];
  double C = coeff[2];
  double D = coeff[3];
  // http://www.ambrsoft.com/TrigoCalc/Plan3D/PlaneLineIntersection_.htm
  double x1 = P1[0];
  double a = (P2[0] - P1[0]);
  double y1 = P1[1];
  double b = (P2[1] - P1[1]);
  double z1 = P1[2];
  double c = (P2[2] - P1[2]);
  double t = -(A * x1 + B * y1 + C * z1 + D) / (A * a + B * b + C * c);

  (intersection)[0] = x1 + a * t;
  (intersection)[1] = y1 + b * t;
  (intersection)[2] = z1 + c * t;

  bool result = (t < 0 || t > 1) ? false : true;  // False if the intersection is with the line P1-P2, not with the
                                                  // segment P1 - P2

  return result;
}