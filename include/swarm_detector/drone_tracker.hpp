#include "opencv2/opencv.hpp"
#include <vector>
#include <opencv2/tracking.hpp>
#include <map>
#include <eigen3/Eigen/Eigen>
#include <camera_model/camera_models/PinholeCamera.h>

static Eigen::Vector3d R2ypr(const Eigen::Matrix3d &R, int degress = true);

#define PIXEL_COEFF 0.5
#define MAX_DRONE_ID 100

struct TrackedDrone {
    int _id;

    cv::Rect2d bbox;
    Eigen::Vector3d unit_p_body;
    Eigen::Vector3d unit_p_cam;
    Eigen::Vector3d unit_p_body_yaw_only;
    double probaility = 1.0;
    double inv_dep = 0;
    Eigen::Vector2d center;
    TrackedDrone() {}

    TrackedDrone(int id, cv::Rect2d _rect, double _inv_dep, double _p):
        _id(id), bbox(_rect), inv_dep(_inv_dep), probaility(_p), center(_rect.x + _rect.width/2.0, _rect.y + _rect.height/2.0)
    {
    }


    //This is self camera position and quat
    void update_position(
        Eigen::Vector3d tic, Eigen::Matrix3d ric, 
        Eigen::Matrix3d Rdrone,
        camera_model::PinholeCameraPtr cam) {
        auto ypr = R2ypr(Rdrone, false);
        double yaw = ypr.x();
        Eigen::Vector3d p3d;
        cam->liftProjective(center, p3d);
        unit_p_cam = p3d.normalized();
        unit_p_body = ric * p3d;
        unit_p_body.normalize();

        //No scale so assume camera is on CG
        unit_p_body_yaw_only = Rdrone*unit_p_body;
        unit_p_body_yaw_only = Eigen::AngleAxisd(-yaw, Eigen::Vector3d::UnitZ()) * unit_p_body_yaw_only;
        unit_p_body_yaw_only.normalize();
    }

    //Return a virtual distance
    Eigen::Vector2d distance_to_drone(Eigen::Vector3d _pos, Eigen::Vector3d tic, Eigen::Matrix3d ric, Eigen::Matrix3d Rdrone, double focal_length = 256, double scale = 0.6) {
        std::cout << "EST Pos" <<  _pos.transpose() << std::endl;
        Eigen::Vector3d d_body = _pos - tic;
        double _inv_dep = 1/d_body.norm();
        d_body.normalize();

        double est_bbx_width = _inv_dep*focal_length*scale;
	    ROS_INFO("Inv Dep width est drone (%f,%f) this (%f,%f)", 
                _inv_dep, est_bbx_width,
                inv_dep, this->bbox.width);
        std::cout << "P detection" << unit_p_body << std::endl;
        std::cout << "P estimate" << d_body << std::endl;
        return Eigen::Vector2d(d_body.adjoint()*unit_p_body, 
                est_bbx_width - this->bbox.width);
    }

    //Return a virtual distance
    Eigen::Vector2d distance_to_drone(TrackedDrone tracked_drone) {
        return Eigen::Vector2d(tracked_drone.unit_p_cam.adjoint()*unit_p_cam, bbox.width - tracked_drone.bbox.width);
    }
};



class DroneTracker {

    std::map<int, cv::Ptr<cv::Tracker>> trackers;
    camera_model::PinholeCameraPtr cam; 

    double focal_length;
    std::map<int, TrackedDrone> tracking_drones;

    std::map<int, Eigen::Vector3d> swarm_drones;

    int last_create_id = rand()%1000*100;
    double p_track;

    int match_id(TrackedDrone &tdrone) {

        int best_id = -1;
        double best_cost = 10000;
        bool matched_on_estimate_drone = false;

        //Match with swarm drones
        for(auto & it : swarm_drones) {
            printf("\n\n\n Match [%f,%f](%f) with %d\n", 
                it.first, tdrone.center.x(), tdrone.center.y(), tdrone.bbox.width);
            auto dis2d = tdrone.distance_to_drone(it.second, tic, ric, Rdrone);
            double angle = acos(dis2d.x());

            double d_est = (ric.transpose()*(it.second - tic)).z();
            double w_det = tdrone.bbox.width;
            double pixel_error = fabs(dis2d.y())/w_det;
            double angle_error = fabs(angle*d_est/(drone_scale));


            ROS_INFO("Match with est %d dis [%f, %f] angle [%f] err [%f/%f, %f/%f] D EST %f", it.first, 
		        dis2d.x(), dis2d.y(),
		        angle*180/M_PI, angle_error, accept_direction_thres,
		        pixel_error, accept_inv_depth_thres, 
                d_est) ;


            if (angle_error < accept_direction_thres && pixel_error < accept_inv_depth_thres) {
                if (angle + PIXEL_COEFF*pixel_error < best_cost) {
                    best_cost = angle + PIXEL_COEFF*pixel_error;
                    best_id = it.first;
                    ROS_INFO("Matched on estimate drone %d...", best_id);
                    matched_on_estimate_drone = true;
                }
            }
        }

        int best_id_tracker = -1;

        //Match with trackers
        for (auto & it: tracking_drones) {
            auto dis2d = tdrone.distance_to_drone(it.second);
            double angle = acos(dis2d.x());
            
            //Maybe we should compare only bounding box for tracker drone
            double w_det = tdrone.bbox.width;
            double pixel_error = fabs(dis2d.y())/w_det;
            double angle_error = fabs(angle/(drone_scale*it.second.inv_dep));

            ROS_INFO("Match tracker %d dis [%f, %f] err [%f/%f, %f/%f]", it.first, 
		        dis2d.x(), dis2d.y(),
		        angle*180.0/M_PI, accept_direction_thres,
		        pixel_error, accept_inv_depth_thres);

            if (angle < accept_direction_thres && pixel_error < accept_inv_depth_thres) {
                if (angle + PIXEL_COEFF*pixel_error < best_cost) {
                    if (matched_on_estimate_drone && it.first < MAX_DRONE_ID)
                    best_cost = angle + PIXEL_COEFF*pixel_error;
                    best_id_tracker = it.first;
                    ROS_INFO("Matched on tracker drone %d...", best_id);
                }
            }
        }

        if (matched_on_estimate_drone) {
            if (best_id_tracker > 0 ) {
                ROS_INFO("Track %d is same with drone %d", best_id_tracker, best_id);
                trackers.erase(best_id_tracker);
                tracking_drones.erase(best_id_tracker);
            }
        } else {
            best_id = best_id_tracker;
        }

        return best_id;
    }


    std::pair<cv::Rect2d, int> convert_rect2d(cv::Rect2d rect) {
        // int side_pos_id = floor(rect / top_size.width) + 1;
    }

    bool update_bbox(cv::Rect2d rect, double p, cv::Mat & frame, TrackedDrone & drone) {
        //Simple use width as scale
        //Depth = f*DroneWidth(meter)/width(pixel)
        //InvDepth = width(pixel)/(f*width(meter))
        drone = TrackedDrone(-1, rect, ((double)rect.width)/(drone_scale*focal_length), p);
        drone.update_position(tic, ric, Rdrone, cam);

        int _id = match_id(drone);

        if(_id < 0) {
            if(track_matched_only) {
                return false;
            } else {
            //Gives a random id
               last_create_id ++;
                _id = last_create_id;
            }
        } else {
            tracking_drones.erase(_id);
        }

        start_tracker_tracking(_id, frame, rect);

        ROS_INFO("New detected drone: %d", _id);

        drone._id = _id;

        tracking_drones[_id] = drone;
        return true;
    }

    bool track_matched_only = false;

    Eigen::Vector3d Pdrone = Eigen::Vector3d::Zero();
    Eigen::Matrix3d Rdrone = Eigen::Matrix3d::Identity();
    Eigen::Vector3d tic;
    Eigen::Matrix3d ric;

    std::vector<Eigen::Matrix3d> rics;

    double drone_scale;
    double min_p;
    double accept_direction_thres;
    double accept_inv_depth_thres;

    bool is_concat_track = false;
    int single_width = false;
public:

    void update_swarm_pose(std::map<int, Eigen::Vector3d> _swarm_drones) {
        swarm_drones = _swarm_drones;
    }

    void update_cam_pose(Eigen::Vector3d _Pdrone, Eigen::Matrix3d _Rdrone) {
        Pdrone = _Pdrone;
        Rdrone = _Rdrone;
    }

    DroneTracker(Eigen::Vector3d _tic, 
                Eigen::Matrix3d _ric, 
                camera_model::PinholeCameraPtr _cam, 
                double _drone_scale, 
                double _p_track, 
                double _min_p,
                double _accept_direction_thres,
                double _accept_inv_depth_thres,
                bool _track_matched_only):
        tic(_tic), ric(_ric), cam(_cam), focal_length(cam->getParameters().fx()), drone_scale(_drone_scale), p_track(_p_track), min_p(_min_p), 
        accept_direction_thres(_accept_direction_thres),
        accept_inv_depth_thres(_accept_inv_depth_thres),
        track_matched_only(_track_matched_only)
    {
        //std::cout << "Tracker ric" << ric << "tic:" << tic << std::endl;
    }   

    DroneTracker(Eigen::Vector3d _tic, 
                std::vector<Eigen::Matrix3d> _rics, 
                camera_model::PinholeCameraPtr _cam, 
                double _drone_scale, 
                double _p_track, 
                double _min_p,
                double _accept_direction_thres,
                double _accept_inv_depth_thres,
                bool _track_matched_only,
                int _single_width):
        tic(_tic), rics(_rics), cam(_cam), focal_length(cam->getParameters().fx()), drone_scale(_drone_scale), p_track(_p_track), min_p(_min_p), 
        accept_direction_thres(_accept_direction_thres),
        accept_inv_depth_thres(_accept_inv_depth_thres),
        track_matched_only(_track_matched_only), single_width(_single_width),
        is_concat_track(true)
    {
        
    }  

    std::vector<TrackedDrone> track(cv::Mat & _img) {
        std::vector<TrackedDrone> ret;
        std::vector<int> failed_id;
        for (auto & it : trackers) {
            cv::Rect2d rect;
            int _id = it.first;
            bool success = it.second->update(_img, rect);
            if (success) {
                assert(tracking_drones.find(_id)!=tracking_drones.end() && "Tracker not found in tracked drones!");
                auto old_tracked = tracking_drones[_id];
                TrackedDrone TDrone(_id, rect, ((double)rect.width)/(drone_scale*focal_length), old_tracked.probaility*p_track);

                if (TDrone.probaility > min_p) {
                    TDrone.update_position(tic, ric, Rdrone, cam);
                    ret.push_back(TDrone);
                    tracking_drones[_id] = TDrone;
                } else {
                    failed_id.push_back(_id);
                }
            } else {
                failed_id.push_back(_id);
            }
        }

        for (auto _id : failed_id) {
            ROS_INFO("Remove tracker of drone %d", _id);
            trackers.erase(_id);
            tracking_drones.erase(_id);
        }

        return ret;
    }



    std::vector<TrackedDrone> process_detect(cv::Mat & img, std::vector<std::pair<cv::Rect2d, double>> detected_drones) {
        std::vector<TrackedDrone> ret;

        std::vector<TrackedDrone> new_tracked = track(img);

        //We only pub out detected drones; 
        //Tracked drones now is only for matching id
        for (auto rect: detected_drones) {
            TrackedDrone tracked_drones;
            bool success = update_bbox(rect.first, rect.second, img, tracked_drones);
            if(success) {
                ret.push_back(tracked_drones);
            }
        }
        return ret;
    }

    void start_tracker_tracking(int _id, cv::Mat & frame, cv::Rect2d bbox) {
        if (trackers.find(_id) != trackers.end()) {
            trackers.erase(_id);
        }
        auto tracker = cv ::TrackerMOSSE::create();
        // auto tracker = cv::TrackerMedianFlow::create();
        tracker->init(frame, bbox);
        trackers[_id] = tracker;
    }

};

static Eigen::Vector3d R2ypr(const Eigen::Matrix3d &R, int degress)
{
    Eigen::Vector3d n = R.col(0);
    Eigen::Vector3d o = R.col(1);
    Eigen::Vector3d a = R.col(2);

    Eigen::Vector3d ypr(3);
    double y = atan2(n(1), n(0));
    double p = atan2(-n(2), n(0) * cos(y) + n(1) * sin(y));
    double r = atan2(a(0) * sin(y) - a(1) * cos(y), -o(0) * sin(y) + o(1) * cos(y));
    ypr(0) = y;
    ypr(1) = p;
    ypr(2) = r;

    if (degress) {
        return ypr / M_PI * 180.0;
    } else {
        return ypr;
    }

}
