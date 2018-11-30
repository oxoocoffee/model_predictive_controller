#include <math.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "MPC.h"

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Evaluate a polynomial.
double polyeval(Eigen::VectorXd coeffs, double x) {
    double result = 0.0;

    for (int i = 0; i < coeffs.size(); i++) {
        result += coeffs[i] * pow(x, i);
    }

    return result;
}

// Fit a polynomial.
// Adapted from
// https://github.com/JuliaMath/Polynomials.jl/blob/master/src/Polynomials.jl#L676-L716
Eigen::VectorXd polyfit(Eigen::VectorXd xvals,
                        Eigen::VectorXd yvals,
                        int order)
{
    assert(xvals.size() == yvals.size());
    assert(order >= 1 && order <= xvals.size() - 1);

    Eigen::MatrixXd A(xvals.size(), order + 1);

    for (int i = 0; i < xvals.size(); i++) {
        A(i, 0) = 1.0;
    }

    for (int j = 0; j < xvals.size(); j++) {
        for (int i = 0; i < order; i++) {
            A(j, i + 1) = A(j, i) * xvals(j);
        }
    }

    return A.householderQr().solve(yvals);
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    // MPC is initialized here!
    MPC mpc;

    string event = "telemetry";

    if (event == "telemetry")
    {
//        j[1] is the data JSON object
//        vector<double> ptsx = j[1]["ptsx"];
//        vector<double> ptsy = j[1]["ptsy"];
//        double px = j[1]["x"];
//        double py = j[1]["y"];
//        double psi = j[1]["psi"];
//        double v = j[1]["speed"];

        vector<double> ptsx = {0.0, 0.0, 0.0};
        vector<double> ptsy = {0.0, 0.0, 0.0};
        double px           = 0.0;
        double py           = 0.0;
        double psi          = 0.0;
        double v            = 0.0;

        // Compensate for an expected latency of 100 ms
        // by predicting the car's state in 100 ms using
        // the vehicle model.
        //
        double latency = 0.1; // Set the expected latency here.
//      double delta = j[1]["steering_angle"];
        double delta = 0.0;
//        std::cout << "Steering angle from simulator: " << delta << std::endl;
//        double a = j[1]["throttle"]; // Terrible approximation, throttle is not acceleration. But this is all the information we have at hand.
        double a = 0.0;
        double Lf = 2.67;
        v *= 0.44704; // The model needs the velocity converted from mph to meters per second
        px += v * cos(psi) * latency;
        py += v * sin(psi) * latency;
        psi -= v * delta / Lf * latency;
        v += a * latency;

        // Convert waypoints to vehicle coordinates.
        //
        // Note: Transforming from map coordinates to vehicle coordinates is the
        // inverse operation of the transformation applied in the particle filter,
        // where the vehicle's observations were transformed from vehicle coordinates
        // to map coordinates. The order of this inverse transformation is translation
        // first, rotation second. The inverse rotation matrix is:
        // [ cos(psi) sin(psi)]
        // [-sin(psi) cos(psi)]
        //
        // The transformation is as follows:
        //
        // x_waypoint_vehicle = (x_waypoint_map - x_vehicle_map) * cos(psi) + (y_waypoint_map - y_vehicle_map) * sin(psi)
        // y_waypoint_vehicle = -(x_waypoint_map - x_vehicle_map) * sin(psi) + (y_waypoint_map - y_vehicle_map) * cos(psi)
        //

        vector<double> ptsx_vehicle = vector<double>(ptsx.size());
        vector<double> ptsy_vehicle = vector<double>(ptsy.size());

        for (vector<double>::size_type i = 0; i < ptsx.size(); i++) {
            double x_trans = ptsx[i] - px;
            double y_trans = ptsy[i] - py;
            ptsx_vehicle[i] = x_trans * cos(psi) + y_trans * sin(psi);
            ptsy_vehicle[i] = -x_trans * sin(psi) + y_trans * cos(psi);
        }

        // Fit a polynomial.
        //
        // Now that the waypoints were transformed into vehicle coordinates,
        // we'll fit a polynomial to them. This will be our reference trajectory
        // from vehicle perspective.
        //
        // Before we can fit the polynomial, we need to convert the STL vectors into
        // Eigen::VectorXd, which the polyfit() function wants as input.
        //
        Eigen::VectorXd xvals = Eigen::VectorXd(ptsx_vehicle.size());
        Eigen::VectorXd yvals = Eigen::VectorXd(ptsy_vehicle.size());

        for (vector<double>::size_type i = 0; i < ptsx_vehicle.size(); i++) {
            xvals(static_cast<int>(i)) = ptsx_vehicle[i];
            yvals(static_cast<int>(i)) = ptsy_vehicle[i];
        }

        Eigen::VectorXd coeffs = polyfit(xvals, yvals, 3);

        // Compute the initial cross-track and orientation errors.
        //
        double cte = polyeval(coeffs, 0);
        // The desired orientation is tangent to the reference trajectory at x = 0.
        double df_0 = coeffs[1]; // Derivative of the polynomial evaluated at x = 0
        double psides0 = atan(df_0); // The desired angle
        // Since the vehicle's current orientation angle from its own perspective is 0 degrees,
        // the orientation error is just the additive inverse of the desired angle.
        double epsi = -psides0;

        // Define the current state.
        // Since the state is in vehicle coordinates, the vehicle's current position from
        // its own perspective is always x = y = psi = 0.
        // The simulator returns the speed in mph, but we want it in meters per second (mps), so we convert it.
        // double v_mps = v * 0.44704;
        Eigen::VectorXd state(6);
        state << 0.0, 0.0, 0.0, v, cte, epsi;

        // Run the optimizer.
        vector<double> result = mpc.Solve(state, coeffs);

        // Set the steering and throttle values.
        // Note: The steering value needs to be divided by deg2rad(25),
        // otherwise the value would be in [-deg2rad(25), deg2rad(25)] instead of [-1, 1],
        // because we set the boundaries of the steering angle in the optimizer to
        // [-deg2rad(25), deg2rad(25)].
//        double steer_value = -result[0] / deg2rad(25);
//        double throttle_value = result[1];

//        json msgJson;

//        msgJson["steering_angle"] = steer_value;
//        msgJson["throttle"] = throttle_value;

          //Display the MPC predicted trajectory

        vector<double> mpc_x_vals;
        vector<double> mpc_y_vals;

        for (vector<double>::size_type i = 2; i < result.size(); i++) {
            if (i%2 == 0)
                mpc_x_vals.push_back(result[i]);
            else
                mpc_y_vals.push_back(result[i]);
        }

        // Add (x,y) points to list here, points are in reference to the vehicle's coordinate system
        // the points in the simulator are connected by a Green line

//        msgJson["mpc_x"] = mpc_x_vals;
//        msgJson["mpc_y"] = mpc_y_vals;

        //Display the waypoints/reference line

        // Add (x,y) points to list here, points are in reference to the vehicle's coordinate system
        // the points in the simulator are connected by a Yellow line

        vector<double> ref_x_vals;
        vector<double> ref_y_vals;
        double ref_points_dist = 2.5;
        int num_ref_points = 25;

        for (int i = 1; i < num_ref_points; i++) {
            double ref_x = i * ref_points_dist;
            ref_x_vals.push_back(ref_x);
            ref_y_vals.push_back(polyeval(coeffs, ref_x));
        }

//        msgJson["next_x"] = ref_x_vals;
//        msgJson["next_y"] = ref_y_vals;


//        auto msg = "42[\"steer\"," + msgJson.dump() + "]";
//        std::cout << msg << std::endl;
        // Latency
        // The purpose is to mimic real driving conditions where
        // the car does actuate the commands instantly.
        //
        this_thread::sleep_for(chrono::milliseconds(100));
//        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
    }
    else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
//        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
    }
}
