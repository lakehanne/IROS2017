/*
        Olalekan Ogunmolu.
        June 01, 2017

        See: A Model for Registration of 3D shapes,
             Paul Besl and Neil D. McKay

             Eqs 23 - 27
*/

#include "ros/ros.h"
#include <ros/spinner.h>
#include "std_msgs/String.h"
#include "tf_conversions/tf_eigen.h"
#include <tf/transform_broadcaster.h>
#include <tf/transform_datatypes.h>  //for bt Quaternion
#include <geometry_msgs/Transform.h>

#include <mutex>
#include <vector>
#include <thread>
#include <typeinfo>

#include <vicon_bridge/Markers.h>
#include <geometry_msgs/Point.h>

#include <Eigen/Eigenvalues>
// for sender to RIO
#include <ensenso/boost_sender.h>

using namespace Eigen;
#define OUT(__o__) std::cout<< __o__ << std::endl;

//used to retrieve value from the ros param server
double get(
    const ros::NodeHandle& n,
    const std::string& name) {
    double value;
    n.getParam(name, value);
    return value;
}

class Receiver
{
private:
    int count, num_points; //iterator and number of markers on object
    std::vector<geometry_msgs::Point> headMarkersVector,
                                    firstHeadMarkersVector;
    std::vector<Vector3d> face_vec, first_face_vec;
    // identity matrix involved in Q matrix
    Matrix3d I3;

    // covariance matrix
    Matrix<double, 3, 3> sigma_px, temp, rotation_matrix;
    Matrix<double, 3, 3> A_Mat;

    // Form Q from which we compute the rotation quaternion
    Matrix4d Q;
    // Delta
    Vector3d Delta;
    // will contain rotationand translation of the head
    geometry_msgs::Pose pose_info;

    ros::NodeHandle nm_;
    std::mutex mutex;
    bool updatePose, running, print_;
    ros::AsyncSpinner spinner;
    unsigned long const hardware_threads;

    // pose vector
    ros::Subscriber sub_markers;
    ros::Publisher pose_pub; //publisher for translation and euler angles

    std::thread rotoTransThread;
    double roll, pitch, yaw;
    Vector3d mu_p, mu_x;  // average of points
    geometry_msgs::Point translation_vec_optim; // optimal translation vector
    double x, y, z;
    double q1, q2, q3, q4;

    // sender objects 
    boost::asio::io_service io_service;
    const std::string multicast_address;

public:
    Receiver(const bool& print)
    :  hardware_threads(std::thread::hardware_concurrency()),
       spinner(2), count(0), num_points(4), updatePose(false), print_(print),
       multicast_address("235.255.0.1")
    {
       I3.setIdentity(3, 3);
    }

    ~Receiver()
    {
        // rotoTransThread.detach();
    }

    Receiver(Receiver const&) =delete;
    Receiver& operator=(Receiver const&) = delete;

    void run()
    {
      spawn();
      unspawn();
    }
private:
    void spawn()
    {
        if(spinner.canStart())
            spinner.start();
        running = true;
        pose_pub = nm_.advertise<geometry_msgs::Pose>("/mannequine_head/pose", 1000);

        sub_markers = nm_.subscribe("/vicon/markers", 10, &Receiver::callback, this);
        while(!updatePose) {
            if(!ros::ok()) {
              return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // spawn the threads
        rotoTransThread = std::thread(&Receiver::processRotoTrans, this);
        if(rotoTransThread.joinable())
            rotoTransThread.join();
    }

    void unspawn()
    {
        spinner.stop();
        rotoTransThread.detach();
        running = false;
    }

    void callback(const vicon_bridge::MarkersConstPtr& markers_msg)
    {
        // solve all vicon markers here
        std::vector<geometry_msgs::Point> headMarkersVector;
        headMarkersVector.resize(num_points);
        for(auto i=0; i < num_points; ++i){
            headMarkersVector[i] = markers_msg -> markers[i].translation;   // fore
        }

        std::lock_guard<std::mutex> lock(mutex);
        this->headMarkersVector = headMarkersVector;
        updatePose          = true;
        ++count;
    }

    void remove_mean(std::vector<geometry_msgs::Point> && vec, Vector3d&& mu)    {
        double mu_x = 0, mu_y = 0, mu_z = 0;
        // std::cout << "mu_x: " << mu_x << " mu_y: " << mu_y << " mu_z: " << mu_z << std::endl;
        for(auto i = 0; i < num_points; ++i)        {
            mu_x += vec[i].x;
            mu_y += vec[i].y;
            mu_z += vec[i].z;
        }

        mu_x /= num_points;
        mu_y /= num_points;
        mu_z /= num_points;

        mu << mu_x, mu_y, mu_z;

        for(auto i = 0; i < num_points; ++i)        {
            vec[i].x -= mu_x;
            vec[i].y -= mu_y;
            vec[i].z -= mu_z;
            // ROS_INFO("vec[%d].z: %.3f", i, headMarkersVector[i].z);
        }
    }

    void point_to_eigen(std::vector<geometry_msgs::Point>&& pt, std::vector<Vector3d>&& face_vec)    {
        for(auto i=0; i < num_points; ++i){
            face_vec[i] << pt[i].x, pt[i].y, pt[i].z;
        }
    }


    // this closely follows pg 243 of the ICP paper by Besl and McKay
    void processRotoTrans()    {
        std::vector<geometry_msgs::Point> headMarkersVector;
        headMarkersVector.resize(num_points);
        firstHeadMarkersVector.resize(num_points);
        first_face_vec.resize(num_points);
        bool use_hard_coded = get(nm_, "/vicon_icp/Utils/use_hard_coded");

        if(use_hard_coded){            
            // seems better we hardcode these values in a yaml file for now
            firstHeadMarkersVector[0].x = get(nm_, "/vicon_icp/BasePose/Marker_Fore/x");
            firstHeadMarkersVector[0].y = get(nm_, "/vicon_icp/BasePose/Marker_Fore/y");
            firstHeadMarkersVector[0].z = get(nm_, "/vicon_icp/BasePose/Marker_Fore/z");

            firstHeadMarkersVector[1].x = get(nm_, "/vicon_icp/BasePose/Marker_Left/x");
            firstHeadMarkersVector[1].y = get(nm_, "/vicon_icp/BasePose/Marker_Left/y");
            firstHeadMarkersVector[1].z = get(nm_, "/vicon_icp/BasePose/Marker_Left/z");

            firstHeadMarkersVector[2].x = get(nm_, "/vicon_icp/BasePose/Marker_Right/x");
            firstHeadMarkersVector[2].y = get(nm_, "/vicon_icp/BasePose/Marker_Right/y");
            firstHeadMarkersVector[2].z = get(nm_, "/vicon_icp/BasePose/Marker_Right/z");

            firstHeadMarkersVector[3].x = get(nm_, "/vicon_icp/BasePose/Marker_Chin/x");
            firstHeadMarkersVector[3].y = get(nm_, "/vicon_icp/BasePose/Marker_Chin/y");
            firstHeadMarkersVector[3].z = get(nm_, "/vicon_icp/BasePose/Marker_Chin/z");
        }
        else{
            if(count==7){
                firstHeadMarkersVector = this->headMarkersVector;
            }
        }

        this->mu_p.resize(3); 
        this->mu_x.resize(3);
        remove_mean(std::move(firstHeadMarkersVector), std::move(this->mu_x));  // mu_x is the model point set
        //convert from geometry points to eigen
        point_to_eigen(std::move(firstHeadMarkersVector), std::move(first_face_vec));

        for(; running && ros::ok() ;)
        {    
       
            if(updatePose)
            {

                // ROS_INFO_STREAM("model point set: " << this->mu_x(0) << " | " <<  this->mu_x(1) << " | " << this->mu_x(2));
                // ROS_INFO_STREAM("measured point set: " << this->mu_p(0) << " | " <<  this->mu_p(1) << " | " << this->mu_p(2));
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    headMarkersVector = this->headMarkersVector;
                    updatePose = false;                    
                }

                //compute center of mass of model and measured point set
                remove_mean(std::move(headMarkersVector), std::move(this->mu_p));  // mu_p is the measured point set
                //convert from geometry points to eigen
                face_vec.resize(num_points);
                point_to_eigen(std::move(headMarkersVector), std::move(face_vec));

                // for(auto elem: face_vec)
                //     ROS_INFO_STREAM("face_vec: " << elem.transpose());
                // for(auto elem: first_face_vec)
                //     ROS_INFO_STREAM("first_face_vec: " << elem.transpose());
                //compute the cross covariance matrix of the points sets P and X
                sigma_px.resize(3, 3); // sigma_px will be 3x3 after the multiplication below

                sigma_px =  first_face_vec[0] * face_vec[0].transpose() +
                            first_face_vec[1] * face_vec[1].transpose() +
                            first_face_vec[2] * face_vec[2].transpose() +
                            first_face_vec[3] * face_vec[3].transpose() ;
                sigma_px /= num_points;
                // ROS_INFO_STREAM("\nsigma_px: \n" << sigma_px);
                
                // A will be 3x3 skew symmetric
                A_Mat.resize(3, 3);
                A_Mat = sigma_px - sigma_px.transpose(); 
                // ROS_INFO_STREAM("\nA_Mat: \n" << A_Mat);

                //collect cyclic components of skew symmetric matrix
                Delta << A_Mat(1,2), A_Mat(2, 0), A_Mat(0, 1); // will be of size 3x1
                // ROS_INFO_STREAM("\nDelta: \n" << Delta.transpose());

                temp.resize(3, 3);  // will be 3x3
                temp = sigma_px + sigma_px.transpose() - (sigma_px.trace() * I3);
                // ROS_INFO_STREAM("\ntemp: \n" << temp);

                // Form the symmetric 4x4 Q matrix
                Q(0, 0) =  sigma_px.trace();                  Q.block<1, 3>(0, 1) = Delta.transpose().eval(); //top row, last three entries
                Q.block<3, 1>(1, 0) = Delta;                  Q.bottomRightCorner<3, 3>() = temp;
                // ROS_INFO_STREAM("\nQ: \n" << Q);

                // we now find the maximum eigen value of the matrix Q
                EigenSolver<Matrix4d> eig(Q);

                // Note that eigVal and eigVec are std::complex types. To access their
                // real or imaginary parts, call real or imag
                EigenSolver< Matrix4d >::EigenvalueType eigVals = eig.eigenvalues();
                EigenSolver< Matrix4d >::EigenvectorsType eigVecs = eig.eigenvectors();

                // ROS_INFO_STREAM("eigVals: " << eigVals[0].real() << ", " << eigVals[1].real() << ", " << eigVals[2].real() << ", " << eigVals[3].real());
                findQuaternion(std::move(eigVals), std::move(eigVecs));
            }
        }
    }

    inline void rad2deg(double&& rad) {
        rad = (rad * 180.0)/M_PI;
    }

    void findQuaternion(EigenSolver< Matrix4d >::EigenvalueType&& eigVals, EigenSolver< Matrix4d >::EigenvectorsType && eigVecs)
    {
        //create a look-up table of eig vectors and values
        std::vector<double> valueVectors {eigVals[0].real(), eigVals[1].real(), eigVals[2].real(), eigVals[3].real()};

        auto max = valueVectors[0];
        int magicIdx = 0;
        for(int i = 0; i < valueVectors.size(); ++i)        {
            if(valueVectors[i] > max) {
              max = valueVectors[i];
              magicIdx = i;
            }
        }
        // find the eigen vector with the largest eigen value, This would be the optimal rotation quaternion
        auto optimalEigVec = eigVecs.col(magicIdx);
        // ROS_INFO_STREAM("\neigVec: \n" << eigVecs);
        // ROS_INFO("optimEigVec: %.4f, %.4f, %.4f, %.4f ", optimalEigVec[0].real(), optimalEigVec[1].real(), optimalEigVec[2].real(), optimalEigVec[3].real());
        // Form optimal rotation quaternion components
        double q0 = optimalEigVec[0].real();
        double q1 = optimalEigVec[1].real();
        double q2 = optimalEigVec[2].real();
        double q3 = optimalEigVec[3].real();

        // tf::Quaternion quart(q0, q1, q2, q3);
        // tf::Matrix3x3 Rot(quart);
        // Rot.getRPY(roll, pitch, yaw);
        // tf::matrixTFToEigen (Rot, rotation_matrix);

        // calculate rotation matrix
        rotation_matrix.resize(3, 3);
        rotation_matrix(0, 0) = std::pow(q0, 2) + std::pow(q1, 2) - std::pow(q2, 2) - std::pow(q3, 2);
        rotation_matrix(0, 1) = 2 * (q1*q2 - q0*q3);
        rotation_matrix(0, 2) = 2 * (q1*q3 + q0*q2);
        rotation_matrix(1, 0) = 2 * (q1*q2 + q0*q3);
        rotation_matrix(1, 1) = std::pow(q0, 2) + std::pow(q2, 2) - std::pow(q1, 2) - std::pow(q3, 2);
        rotation_matrix(1, 2) = 2 * (q2*q3 - q0*q1);
        rotation_matrix(2, 0) = 2 * (q1*q3 - q0*q2);
        rotation_matrix(2, 1) = 2 * (q2*q3 + q0*q1);
        rotation_matrix(2, 2) = std::pow(q0, 2) + std::pow(q3, 2) - std::pow(q1, 2) - std::pow(q2, 2);

        // from https://eigen.tuxfamily.org/dox/group__Geometry__Module.html#gac3d90b12b21e1aaa2a9de9b0e45e6b7c
        // in Vector3f ea = mat.eulerAngles(2, 0, 2), for instance
        // "2" represents the z axis and "0" the x axis, etc
        // This corresponds to the right-multiply conventions (with right hand side frames).
        // The returned angles are in the ranges [0:pi]x[-pi:pi]x[-pi:pi].
        Eigen::Vector3d rpy = rotation_matrix.eulerAngles(0, 1, 2);
        roll    = rpy(0);   // roll is about axis x
        pitch   = rpy(1);   // pitch is about axis z
        yaw     = rpy(1);   // yaw about axis y

        // see this: http://www.staff.city.ac.uk/~sbbh653/publications/euler.pdf
        // theta =rot about x== roll
        // psi = rot about y == pitch
        // phi = rot about z == yaw
        // double roll2, pitch2, yaw2;
        // if (rotation_matrix(2,0) != 1.0 || rotation_matrix(2,0) != -1.0){
        //     roll = -std::asin( rotation_matrix(2,0) );
        //     pitch = std::atan2( rotation_matrix(2,1)/std::cos(roll), rotation_matrix(2,2)/std::cos(roll) );
        //     yaw   = std::atan2( rotation_matrix(1,0)/std::cos(roll), rotation_matrix(0,0)/std::cos(roll) );

        //     roll2  = M_PI - roll;
        //     pitch2 = std::atan2( rotation_matrix(2,1)/std::cos(roll2), rotation_matrix(2,2)/std::cos(roll2) );
        //     yaw2   = std::atan2( rotation_matrix(1,0)/std::cos(roll2), rotation_matrix(0,0)/std::cos(roll2) );
        // }
        // else{
        //     yaw = 0;
        //     if (rotation_matrix(2,0) == -1.0){                
        //         roll = M_PI/2.0;
        //         pitch = yaw + std::atan2(rotation_matrix(0,1), rotation_matrix(0,2));
        //     }
        //     else{
        //         roll = -M_PI/2.0;
        //         pitch = -yaw + std::atan2(-rotation_matrix(0,1), -rotation_matrix(0,2));
        //     }
        // }
        // convert rads to degrees
        rad2deg(std::move(roll));
        rad2deg(std::move(pitch));
        rad2deg(std::move(yaw));
        // ROS_INFO("[(roll, roll2), (pitch, pitch2): , (yaw, yaw2)]: [(%.4f, %.4f), (%.4f, %.4f), (%.4f, %.4f)]", roll, roll2, pitch,pitch2, yaw, yaw2);

        Vector3d optimal_trans = (this->mu_p - /*rotation_matrix **/ this->mu_x);
        pose_info.position.x = optimal_trans(0); 
        pose_info.position.y = optimal_trans(1);
        pose_info.position.z = this->mu_p(2)-942; 
        // ROS_INFO_STREAM("optimal trans check: " << this->mu_x - rotation_matrix * this->mu_p);

        pose_info.orientation.x = roll;
        pose_info.orientation.y = pitch;
        pose_info.orientation.z = yaw;
        pose_info.orientation.w = 1;

        // publish the head pose
        pose_pub.publish(pose_info);
        if(print_){            
            ROS_INFO("z: %.3f | roll: %.3f | pitch: %.3f | yaw: %.3f \n", pose_info.position.z, \
                                                            pose_info.orientation.x, pose_info.orientation.y, pose_info.orientation.z);
        }
        ros::Rate looper(30);
        looper.sleep();

        // ROS_INFO_STREAM("\nrotation matrix\n" << rotation_matrix);

        // define tf matrix to hold xalculated eigen matrix
        // if(std::fabs(rotation_matrix(0,0)) < 0.001 & std::fabs(rotation_matrix(1, 0)) < .001){
        //     //singularity
        //     roll  = 0 ;
        //     pitch = std::atan2(-rotation_matrix(2,0), rotation_matrix(0,0));
        //     yaw   = std::atan2(-rotation_matrix(1,2), rotation_matrix(1,1));
        // }
        // else{
        //     roll = std::atan2(rotation_matrix(1,0), rotation_matrix(0,0));
        //     pitch = std::atan2(-rotation_matrix(2,0), 
        //                         std::cos(roll) * rotation_matrix(0,0) + std::sin(roll) * rotation_matrix(1,0));
        //     yaw = std::atan2(std::sin(roll) * rotation_matrix(0,2) - std::cos(roll) * rotation_matrix(1,2), 
        //                     std::cos(roll)*rotation_matrix(1,1) - std::sin(roll)*rotation_matrix(0,1));            
        // }        

        // form quaternion from euler angles
        // tf::Quaternion quat = tf::createQuaternionFromRPY(roll, pitch, yaw);
    }
};

int main(int argc, char** argv)
{
    ros::init(argc, argv, "vicon_icp_node");

    if(!ros::ok())   
      return EXIT_SUCCESS;

    ROS_INFO_STREAM("Started node " << ros::this_node::getName().c_str());

    bool print;
    if(!ros::param::get("/vicon_icp/Utils/print", print))
        ROS_DEBUG("could not retrieve print param value from ros parameter server");

    Receiver rcvr(print);
    rcvr.run();

    ros::shutdown();
    return EXIT_SUCCESS;
}
