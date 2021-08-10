//
// Created by ziyi on 7/8/21.
//

#include <ros/ros.h>
#include <unistd.h>
#include <behaviortree_cpp_v3/bt_factory.h>
#include <behaviortree_cpp_v3/loggers/bt_zmq_publisher.h>
#include <behaviortree_cpp_v3/loggers/bt_file_logger.h>
#include <geometry_msgs/PoseStamped.h>
#include <move_base_msgs/MoveBaseAction.h>
#include <actionlib/client/simple_action_client.h>
#include <std_msgs/builtin_string.h>
#include <std_msgs/Int8.h>
#include <ltl_automaton_msgs/TransitionSystemStateStamped.h>
#include <ltl_automaton_msgs/LTLPlan.h>
#include "navigation_node.h"
#include <yaml-cpp/yaml.h>
#include <ros/package.h>
#include "quadruped_ctrl/locomotion_status.h"

typedef actionlib::SimpleActionClient<move_base_msgs::MoveBaseAction> Client;

using namespace BT;

class LTLA1Planner{
public:
    LTLA1Planner(){
        client_ = std::make_shared<Client>("/move_base", true);
        task_sub_ = nh_.subscribe("/action_plan", 1, &LTLA1Planner::callbackActionSequence, this);
        loco_status_sub_ = nh_.subscribe("/locomotion_status", 1, &LTLA1Planner::callbackLocomotionStatus, this);
        ltl_state_pub_ = nh_.advertise<ltl_automaton_msgs::TransitionSystemStateStamped>("ts_state", 10, true);
        replanning_request_ = nh_.advertise<std_msgs::Int8>("replanning_request", 1);
        init_params();
        create_monitors();
        run();
    }
    ~LTLA1Planner() = default;
    void init_params(){
        std::string package_name = "ltl_automation_a1";
        // Get default tree from param
        auto aaa = ros::package::getPath(package_name);
        bt_filepath = ros::package::getPath(package_name).append("/resources/replanning_tree_1.xml");
//        nh_.getParam("bt_filepath", bt_filepath);
        ROS_INFO("tree file: %s\n", bt_filepath.c_str());

        // Get TS for param
        std::string ts_filepath;
        ts_filepath = ros::package::getPath(package_name).append("/config/example_ts_dog.yaml");
//        nh_.getParam("transition_system_textfile", ts_filepath);
        transition_system_ = YAML::LoadFile(ts_filepath);

        // Init ltl state message with TS
        ltl_state_msg_.ts_state.state_dimension_names = transition_system_["state_dim"].as<std::vector<std::string>>();

        // Init locomotion status: 0: current_FSM; 1: operating mode
        loco_status = std::vector<std::string>(2, "NONE");

        // Initialize the flags for the replanning logic
        is_first = true;
        replan = false;

    }

    void create_monitors(){
        int number_of_dimensions = transition_system_["state_dim"].size();
        current_ltl_state_ = std::vector<std::string>(number_of_dimensions, "NONE");
        previous_ltl_state_ = current_ltl_state_;

        for(int i=0; i<number_of_dimensions; i++){
            auto dimension = transition_system_["state_dim"].as<std::vector<std::string>>()[i];
            if(dimension == "2d_pose_region"){
                a1_region_sub_ = nh_.subscribe("current_region", 100, &LTLA1Planner::region_state_callback, this);
            } else if (dimension == "a1_load") {
                current_ltl_state_[i] = "unloaded";
            } else {
                std::cout <<"state type " << dimension << " is not supported by LTL A1" << std::endl;
            }
        }

    }

    void run(){
        // set up the blackboard to cache the lower level codes running status
        ros::Rate loop_rate(1);
        factory_.registerNodeType<BTNav::MoveAction>("MoveAction");
        factory_.registerNodeType<BTNav::LTLPreCheck>("LTLPreCheck");
        factory_.registerNodeType<BTNav::UpdateLTL>("UpdateLTL");
        factory_.registerNodeType<BTNav::LocomotionStart>("LocomotionStart");
        factory_.registerNodeType<BTNav::LocomotionStatusCheck>("LocomotionStatusCheck");
        factory_.registerNodeType<BTNav::RecoveryStand>("RecoveryStand");
        factory_.registerNodeType<BTNav::ReplanningRequestLevel2>("ReplanningRequestLevel2");
        factory_.registerNodeType<BTNav::ReplanningRequestLevel3>("ReplanningRequestLevel3");

        my_blackboard_->set("move_base_finished", false);
        my_blackboard_->set("move_base_idle", false);
        my_blackboard_->set("nav_goal", "NONE");
        my_blackboard_->set("ltl_state_current", "NONE");
        my_blackboard_->set("ltl_state_desired_sequence", "NONE");
        my_blackboard_->set("action", "NONE");
        my_blackboard_->set("action_sequence", "NONE");
        my_blackboard_->set("num_cycles", 1);
        my_blackboard_->set("locomotion_status", "NONE");
        my_blackboard_->set("replanning_request", 0);
        my_blackboard_->debugMessage();

//        auto tree = std::make_unique<BT::Tree>();
        auto tree = std::make_unique<BT::Tree>(factory_.createTreeFromFile(bt_filepath, my_blackboard_));
        auto zmq_publisher = std::make_unique<PublisherZMQ>(*tree);
        NodeStatus status = NodeStatus::RUNNING;

        // Send the initial LTL state
        ltl_state_msg_.header.stamp = ros::Time::now();
        ltl_state_msg_.ts_state.states = current_ltl_state_;
        ltl_state_pub_.publish(ltl_state_msg_);

        while(ros::ok()){

            if(is_first && replan){
//                tree = std::make_unique<BT::Tree>(factory_.createTreeFromFile(bt_filepath, my_blackboard_));
//                zmq_publisher = std::make_unique<PublisherZMQ>(*tree);
                is_first = false;
                replan = false;
            } else if (!is_first && replan) {
                zmq_publisher.reset();
                status = NodeStatus::RUNNING;
                tree = std::make_unique<BT::Tree>(factory_.createTreeFromFile(bt_filepath, my_blackboard_));
                zmq_publisher = std::make_unique<PublisherZMQ>(*tree);
                ROS_WARN("BEHAVIOR TREE RELOADED");
                replan = false;
            } else if (is_first && !replan){
                ROS_INFO("Wait for the action sequence to be sent from LTL planner");
                ros::spinOnce();
                loop_rate.sleep();
                continue;
            }
            // update input
            bool move_base_finished = false;
            bool move_base_idle = false;

            // update input
            if (client_->isServerConnected())
            {
                switch (client_->getState().state_)
                {
                    case actionlib::SimpleClientGoalState::REJECTED:
                    case actionlib::SimpleClientGoalState::PREEMPTED:
                    case actionlib::SimpleClientGoalState::ABORTED:
                    case actionlib::SimpleClientGoalState::LOST:
                        move_base_idle = true;
                        break;
                    case actionlib::SimpleClientGoalState::SUCCEEDED:
                        move_base_finished = true;
                        move_base_idle = true;
                        break;
                    case actionlib::SimpleClientGoalState::PENDING:
                    case actionlib::SimpleClientGoalState::ACTIVE:
                    case actionlib::SimpleClientGoalState::RECALLED:
                    default:
                        break;
                }
            }

            my_blackboard_->set("move_base_finished", move_base_finished);
            my_blackboard_->set("move_base_idle", move_base_idle);

            // bt
            if(status != NodeStatus::SUCCESS) {
                status = tree->tickRoot();
                std::string action;
                std::string nav_goal;
                my_blackboard_->get(std::string("action"), action);
                my_blackboard_->get(std::string("nav_goal"), nav_goal);

                // output
                YAML::Node action_dict;
                bool sanity_check1 = false;
                if (client_->isServerConnected()) {
                    if (action == "MOVE_COMMAND") {
                        if (nav_goal == "NONE") {
                            ROS_ERROR("No goal is set");
                        } else {
                            for (YAML::const_iterator iter = transition_system_["actions"].begin();
                                 iter != transition_system_["actions"].end(); ++iter) {
                                if (iter->first.as<std::string>() == nav_goal) {
                                    action_dict = transition_system_["actions"][iter->first.as<std::string>()];
                                    sanity_check1 = true;
                                    break;
                                }
                            }

                            if (!sanity_check1) {
                                ROS_ERROR("next_move_cmd not found in LTL A1 transition system");
                            }

                            a1_action(action_dict);
                        }
                    }
                }
            }

            // Publish ltl current state back to the ltl planner
            if(current_ltl_state_ != previous_ltl_state_) {
                previous_ltl_state_ = current_ltl_state_;
                ltl_state_msg_.header.stamp = ros::Time::now();
                ltl_state_msg_.ts_state.states = current_ltl_state_;
                ltl_state_pub_.publish(ltl_state_msg_);
            }

            // publish the replanning status and ltl current state back to the ltl planner
            int replanning_stat;
            my_blackboard_->get(std::string("replanning_request"), replanning_stat);
            replanning_status.data = replanning_stat;
            if(replanning_status.data != 0){
//                previous_ltl_state_ = current_ltl_state_;
//                ltl_state_msg_.header.stamp = ros::Time::now();
//                ltl_state_msg_.ts_state.states = current_ltl_state_;
//                ltl_state_pub_.publish(ltl_state_msg_);
                replanning_request_.publish(replanning_status);
            }

            ros::spinOnce();
            loop_rate.sleep();
        }


    }

    void callbackActionSequence(const ltl_automaton_msgs::LTLPlan& msg){
        // The xml changes go here
        auto action = msg.action_sequence;
        auto ts_state = msg.ts_state_sequence;
        std::vector<std::vector<std::string>> desired_state_seq;
        std::vector<std::string> action_sequence;
        desired_state_seq.reserve(ts_state.size());
        action_sequence.reserve(action.size());
        for(const auto& state : ts_state){
            desired_state_seq.push_back(state.states);
        }
        for(const auto& act : action){
            action_sequence.push_back(act);
        }
        my_blackboard_->set("ltl_state_desired_sequence", desired_state_seq);
        my_blackboard_->set("action_sequence", action_sequence);
        my_blackboard_->set("nav_goal", action_sequence[0]);
        my_blackboard_->set("num_cycles", action_sequence.size());
        my_blackboard_->set("replanning_request", 0);

        // TODO: Add if statement based on the current tree status
        if(replan){
            ROS_ERROR("FATAL ERROR; REPLAN FLAG HAS TO BE FALSE TO GET READY FOR NEW PLAN");
        }
        replan = true;
    }

    void a1_action(YAML::Node action_dic){
        // Move command
        plan_index++;
        if(action_dic["type"].as<std::string>() == "move"){
            move_base_msgs::MoveBaseGoal current_goal;
            auto pose = action_dic["attr"]["pose"].as<std::vector<std::vector<double>>>();
            auto region = action_dic["attr"]["region"].as<std::string>();

            current_goal.target_pose.header.frame_id = "map";
            current_goal.target_pose.header.seq = plan_index;
            current_goal.target_pose.header.stamp = ros::Time::now();
            current_goal.target_pose.pose.position.x = pose[0][0];
            current_goal.target_pose.pose.position.y = pose[0][1];

            current_goal.target_pose.pose.orientation.x = pose[1][0];
            current_goal.target_pose.pose.orientation.y = pose[1][1];
            current_goal.target_pose.pose.orientation.z = pose[1][2];
            current_goal.target_pose.pose.orientation.w = pose[1][3];
            client_->sendGoal(current_goal);
        }

        // TODO: other actions
    }

    void region_state_callback(const std_msgs::String& msg){
        current_ltl_state_[0] = msg.data;
        std::cout << current_ltl_state_[0] << std::endl;
        my_blackboard_->set("ltl_state_current", current_ltl_state_);
    }

    void callbackLocomotionStatus(const quadruped_ctrl::locomotion_status& msg){
        loco_status[0] = msg.current_fsm;
        loco_status[1] = msg.operating_mode;
        my_blackboard_->set("locomotion_status", loco_status);
    }





private:
    std::shared_ptr<Client> client_;
    BehaviorTreeFactory factory_;
//    move_base_msgs::MoveBaseGoal current_goal_;
    Blackboard::Ptr my_blackboard_ = Blackboard::create();
    ros::NodeHandle nh_;
    std::string bt_filepath;
    std::vector<std::vector<std::string>> desired_state_seq_;
    std::vector<std::string> current_ltl_state_;
    std::vector<std::string> previous_ltl_state_;
    ltl_automaton_msgs::TransitionSystemStateStamped  ltl_state_msg_;
    YAML::Node transition_system_;
    std::vector<std::string> loco_status;

    ros::Subscriber task_sub_;
    ros::Subscriber a1_region_sub_;
    ros::Subscriber loco_status_sub_;
    ros::Publisher ltl_state_pub_;
    ros::Publisher replanning_request_;
    std_msgs::Int8 replanning_status;

    bool is_first;
    bool replan;

    int plan_index = 0;
};

int main(int argc, char** argv){
    ros::init(argc, argv, "a1_ltl_bt");
    LTLA1Planner runner;
    ros::spin();
    return 0;
}
