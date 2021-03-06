#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"
#include "spline.h"
#include "car.h"
#include "helpers.h"

using namespace std;

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.find_first_of("}");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

double distance(double x1, double y1, double x2, double y2) {
	return sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
}

int ClosestWaypoint(double x, double y, const vector<double> &maps_x, const vector<double> &maps_y) {
	double closestLen = 100000; //large number
	int closestWaypoint = 0;

	for(int i = 0; i < maps_x.size(); i++) {
		double map_x = maps_x[i];
		double map_y = maps_y[i];
		double dist = distance(x,y,map_x,map_y);
		if(dist < closestLen) {
			closestLen = dist;
			closestWaypoint = i;
		}
	}
	return closestWaypoint;
}

int NextWaypoint(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y) {
	int closestWaypoint = ClosestWaypoint(x,y,maps_x,maps_y);

	double map_x = maps_x[closestWaypoint];
	double map_y = maps_y[closestWaypoint];

	double heading = atan2((map_y-y),(map_x-x));

	double angle = fabs(theta - heading);
  angle = min(2*pi() - angle, angle);

  if(angle > pi()/4) {
    closestWaypoint++;
    if (closestWaypoint == maps_x.size()) {
    closestWaypoint = 0;
    }
  }

  return closestWaypoint;
}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y) {
	int next_wp = NextWaypoint(x,y, theta, maps_x,maps_y);

	int prev_wp;
	prev_wp = next_wp-1;
	if(next_wp == 0) {
		prev_wp  = maps_x.size()-1;
	}

	double n_x = maps_x[next_wp]-maps_x[prev_wp];
	double n_y = maps_y[next_wp]-maps_y[prev_wp];
	double x_x = x - maps_x[prev_wp];
	double x_y = y - maps_y[prev_wp];

	// find the projection of x onto n
	double proj_norm = (x_x*n_x+x_y*n_y)/(n_x*n_x+n_y*n_y);
	double proj_x = proj_norm*n_x;
	double proj_y = proj_norm*n_y;

	double frenet_d = distance(x_x,x_y,proj_x,proj_y);

	//see if d value is positive or negative by comparing it to a center point

	double center_x = 1000-maps_x[prev_wp];
	double center_y = 2000-maps_y[prev_wp];
	double centerToPos = distance(center_x,center_y,x_x,x_y);
	double centerToRef = distance(center_x,center_y,proj_x,proj_y);

	if(centerToPos <= centerToRef) {
		frenet_d *= -1;
	}

	// calculate s value
	double frenet_s = 0;
	for(int i = 0; i < prev_wp; i++) {
		frenet_s += distance(maps_x[i],maps_y[i],maps_x[i+1],maps_y[i+1]);
	}

	frenet_s += distance(0,0,proj_x,proj_y);

	return {frenet_s,frenet_d};
}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, const vector<double> &maps_s, const vector<double> &maps_x, const vector<double> &maps_y) {
	int prev_wp = -1;

	while(s > maps_s[prev_wp+1] && (prev_wp < (int)(maps_s.size()-1) )) {
		prev_wp++;
	}

	int wp2 = (prev_wp+1)%maps_x.size();

	double heading = atan2((maps_y[wp2]-maps_y[prev_wp]),(maps_x[wp2]-maps_x[prev_wp]));
	// the x,y,s along the segment
	double seg_s = (s-maps_s[prev_wp]);

	double seg_x = maps_x[prev_wp]+seg_s*cos(heading);
	double seg_y = maps_y[prev_wp]+seg_s*sin(heading);

	double perp_heading = heading-pi()/2;

	double x = seg_x + d*cos(perp_heading);
	double y = seg_y + d*sin(perp_heading);

	return {x,y};
}

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;
  
  //int lane = 1; // centre lane >> Lanes = 0,1,2 & Lane centres = 2,6,10 (left lane is 0)
  //const double SAFE_VELOCITY = 49.5;
  //double velocity = 5.0; // start with 5 & slowly build up to SAFE_VELOCITY when possible
  //const double DISTANCE_AHEAD = 30; // comfortable driving distance in front
  bool first_cycle = true;
  
  /*
  * Ego car
  */
  Car ego_car = Car();
  ego_car.id = EGO_CAR_ID;

  ifstream in_map_(map_file_.c_str(), ifstream::in);

  string line;
  while (getline(in_map_, line)) {
  	istringstream iss(line);
  	double x;
  	double y;
  	float s;
  	float d_x;
  	float d_y;
  	iss >> x;
  	iss >> y;
  	iss >> s;
  	iss >> d_x;
  	iss >> d_y;
  	map_waypoints_x.push_back(x);
  	map_waypoints_y.push_back(y);
  	map_waypoints_s.push_back(s);
  	map_waypoints_dx.push_back(d_x);
  	map_waypoints_dy.push_back(d_y);
  }

  h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,&map_waypoints_dx,&map_waypoints_dy,&first_cycle,&ego_car](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length, uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    //auto sdata = string(data).substr(0, length);
    //cout << sdata << endl;
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);
      if (s != "") {
        auto j = json::parse(s);        
        string event = j[0].get<string>();
        if (event == "telemetry") {
          
          // j[1] is the data JSON object
          
        	// Main car's localization Data
          double car_x = j[1]["x"];
          double car_y = j[1]["y"];
          double car_s = j[1]["s"];
          double car_d = j[1]["d"];
          double car_yaw = j[1]["yaw"];
          double car_speed = j[1]["speed"];

          // Previous path data given to the Planner
          auto previous_path_x = j[1]["previous_path_x"];
          auto previous_path_y = j[1]["previous_path_y"];
          // Previous path's end s and d values 
          double end_path_s = j[1]["end_path_s"];
          double end_path_d = j[1]["end_path_d"];

          // Sensor Fusion Data, a list of all other cars on the same side of the road.
          // A 2D vector of cars and then those cars' [ id, x, y, vx, vy, s, d]: 
          // unique id, x position in map coordinates, y position in map coordinates, 
          // x velocity in m/s, y velocity in m/s, 
          // s position in frenet coordinates, d position in frenet coordinates.
          auto sensor_fusion = j[1]["sensor_fusion"];

          json msgJson;

          vector<double> next_x_vals;
          vector<double> next_y_vals;
          
          // £££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££
          cout << "************START************" << endl;
          int prev_path_size = previous_path_x.size();
                            
          /*
          * Create car objects
          */
          vector<Car> normal_cars;
          for (auto s_f: sensor_fusion) {// go through sensor fusion data
            Car normal_car = Car();
            normal_car.id = s_f[0];
            normal_car.s = s_f[5];         
            double vx =  s_f[3];
            double vy =  s_f[4];
            normal_car.s_dot = sqrt(vx*vx + vy*vy);
            normal_car.d = s_f[6];        
            normal_car.x = s_f[1];
            normal_car.y = s_f[2];
            normal_car.lane = (int)normal_car.d/4;
            
            normal_car.predictTrajectory(prev_path_size);
            //normal_car.printPredictedTrajectoryLastPoint();
            //vector<double> computedState =  normal_car.computeStateAtTime(PREDICTION_HORIZON_TIMESTEPS * PREDICTION_HORIZON_TIME_DELTA);
            //cout << "Computed State At Time T: s=" << computedState[0] << ", s_dot=" << computedState[1] << ", d=" << computedState[3] << endl;
            normal_cars.push_back(normal_car);
          }
           
          /*
          * Ego car
          */
          ego_car.s = ((prev_path_size > 0 ) ? (double)end_path_s : car_s);         
          ego_car.s_dot = (first_cycle ? EGO_CAR_START_VELOCITY : car_speed);
          ego_car.d = ((prev_path_size > 0 ) ? (double)end_path_d : car_d);        
          ego_car.x = ((prev_path_size > 0 ) ? (double)previous_path_x[prev_path_size-1] : car_x);
          ego_car.y = ((prev_path_size > 0 ) ? (double)previous_path_y[prev_path_size-1] : car_y);
          ego_car.lane = (first_cycle ? EGO_CAR_START_LANE : (int)ego_car.d/4);
          ego_car.state = Helpers::KEEP_LANE;
          ego_car.checkProximity(normal_cars);
          ego_car.computeFutureStates();
          //ego_car.printFutureStates();
          ego_car.computeFutureStatesTargetSD(normal_cars, prev_path_size);
          //ego_car.printFutureStatesTargetSD();
          ego_car.generateFutureStatesTrajectory();
          //ego_car.printFutureStatesTrajectoryLastSD();
          //ego_car.printCar();           
                              
          /*
          * Trajectory generation w/o JMT but using spline + method in walkthrough to avoid jerk
          */
          
          // Sparse x,y waypoints evenly spaced at DISTANCE_AHEAD metres used as anchors for spline
          // Later on, more wapoints will be generated via spline's interpolation for a smoother path
          vector<double> ptsx;
          vector<double> ptsy;
          
          // Refeence x,y,yaw to be used later
          // These are either set to either current state or previous (if we have prev state)
          double ref_x = car_x;
          double ref_y = car_y;
          double ref_yaw = deg2rad(car_yaw);

          if(prev_path_size == 0) { // if the simulator utilised all points in the previous run or the first run
            double prev_car_x = car_x - cos(car_yaw); // calculate previous x
            double prev_car_y = car_y - sin(car_yaw); // calculate previous y
            
            ptsx.push_back(prev_car_x);
            ptsy.push_back(prev_car_y);
            
            ptsx.push_back(car_x);
            ptsy.push_back(car_y);
            //cout << "prev_path_size == 0" << endl;
          }
          else if(prev_path_size == 1) { // only 1 point left from previous run             
            ptsx.push_back(previous_path_x[0]); // get previous x
            ptsy.push_back(previous_path_y[0]); // get previous y
            
            ptsx.push_back(car_x);
            ptsy.push_back(car_y);
            //cout << "prev_path_size == 1" << endl;
          }
          else {// atleast 2 previous points available so use prev path's last point as the starting point
            // Update ref with the last point
            ref_x = previous_path_x[prev_path_size-1];  
            ref_y = previous_path_y[prev_path_size-1];
            
            double ref_prev_x = previous_path_x[prev_path_size-2];
            double ref_prev_y = previous_path_y[prev_path_size-2];
            ref_yaw = atan2(ref_y-ref_prev_y, ref_x-ref_prev_x); // find the angle tangent to last point
            
            ptsx.push_back(ref_prev_x);
            ptsy.push_back(ref_prev_y);
            
            ptsx.push_back(ref_x);
            ptsy.push_back(ref_y);
            //cout << "prev_path_size > 1" << endl;
          }

          //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
          Helpers::State target_state = Helpers::KEEP_LANE;
          vector<double> s_trajectory;
          vector<double> d_trajectory;
          for (auto future_state_trajectory : ego_car.future_states_trajectory) {
            Helpers::State state = future_state_trajectory.first;
            if (state == target_state){
              vector<vector<double>> trajectory = future_state_trajectory.second;
              s_trajectory = trajectory[0];
              d_trajectory = trajectory[1];
            }
          }
          double velocity = ego_car.future_states_target_sd[target_state][1] ;
          //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
          
          // Add 3 points that are PREDICTION_HORIZON_TIMESTEPS metres apart in s,d coord & then get the corresponding x,y pair
          vector<double> next_wp0 = getXY(s_trajectory[s_trajectory.size()-1], d_trajectory[d_trajectory.size()-1], map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector<double> next_wp1 = getXY(s_trajectory[s_trajectory.size()-1]+30, d_trajectory[d_trajectory.size()-1], map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector<double> next_wp2 = getXY(s_trajectory[s_trajectory.size()-1]+60, d_trajectory[d_trajectory.size()-1], map_waypoints_s, map_waypoints_x, map_waypoints_y);
          
          ptsx.push_back(next_wp0[0]);
          ptsy.push_back(next_wp0[1]);
          
          ptsx.push_back(next_wp1[0]);
          ptsy.push_back(next_wp1[1]);
          
          ptsx.push_back(next_wp2[0]);
          ptsy.push_back(next_wp2[1]);

          
          
          // Shift car ref angle to 0 degrees for easy caculations
          for(int i = 0; i < ptsx.size(); i++) { 
            //cout << "Anchor points (unshifted) " << i << ": x=" << ptsx[i] << ", y=" << ptsy[i] << endl;
            double shift_x = ptsx[i] - ref_x;
            double shift_y = ptsy[i] - ref_y;
            
            ptsx[i] = shift_x*cos(0-ref_yaw) - shift_y*sin(0-ref_yaw);
            ptsy[i] = shift_x*sin(0-ref_yaw) + shift_y*cos(0-ref_yaw); 
            //cout << "Anchor points (shifted) " << i << ": x=" << ptsx[i] << ", y=" << ptsy[i] << endl;
          }
          
          
          //if (prev_path_size > 0) cout << "Previous path first coords: x=" << previous_path_x[0] << ", y=" << previous_path_y[0] << endl;
          //if (prev_path_size > 0) cout << "Previous path last coords: x=" << previous_path_x[prev_path_size-1] << ", y=" << previous_path_y[prev_path_size-1] << endl;
          
          tk::spline s;
          s.set_points(ptsx, ptsy); // specify anchor points (5 in total)
          
          // Start with adding points not utilised last time i.e. the left over points (the simulator doesn't always utilise all the points)
          for(int i = 0; i < previous_path_x.size(); i++) {
            next_x_vals.push_back(previous_path_x[i]);
            next_y_vals.push_back(previous_path_y[i]);
          }
          
          // Calculate how to break up spline points so that we achieve desired velocity + jerk free trajectory
          double target_x = EGO_CAR_SAFE_DISTANCE;
          double target_y = s(target_x); // use spline to get the corresponding y
          double target_dist = sqrt(pow(0-target_x,2) + pow(0-target_y,2));
                    
          double x_add_on = 0; // origin where the car is now but will keep on changing as we add more points
          
          // target_distance = N*0.02*veloctiy OR N = target_distance/(0.02*veloctiy)
          // N is the no. of segments required from origin to target_distance when travelling at velocity
          // 0.02 is used because the simulator picks a point every 0.02 sec
          // 2.24 is the conversion factor for miles/hour to meter/sec
          double N = target_dist/(0.02*velocity/2.24); // N is the total no. of road sections from current location to PREDICTION_HORIZON_TIMESTEPS metres ahead
          double x_increment = target_x/N; // length of each road section in x-axis
          // Add new x,y points to complete 50 points
          for (int i = 1; i <= 50-previous_path_x.size(); i++) { 
            double x_point = x_add_on + x_increment; // start location + length of each road section in x-axis
            double y_point = s(x_point); // find corresponding y 
            
            x_add_on = x_point; // current point becomes the start for next point generation in the loop
            
            double x_ref = x_point;
            double y_ref = y_point;
            
            // rotate back to global coord from local coord
            x_point = x_ref*cos(ref_yaw) - y_ref*sin(ref_yaw);
            y_point = x_ref*sin(ref_yaw) + y_ref*cos(ref_yaw);
            
            x_point += ref_x; // keep the car ahead of the last point
            y_point += ref_y;
            
            next_x_vals.push_back(x_point);
            next_y_vals.push_back(y_point);
            
            //cout << "Newly added point " << i << " coords: x=" << x_point << ", y=" << y_point << endl;
          }
          ego_car.printCar();
          first_cycle = false;
          cout << "************END************" << endl;
          // £££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££££

          // TODO: define a path made up of (x,y) points that the car will visit sequentially every .02 seconds
          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"control\","+ msgJson.dump()+"]";

          //this_thread::sleep_for(chrono::milliseconds(1000));
          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the program doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data, size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code, char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } 
  else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}