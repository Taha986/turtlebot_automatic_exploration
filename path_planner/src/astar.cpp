#include "astar.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <boost/thread.hpp>

#include <nav_msgs/Path.h>
#include <ros/package.h>

using namespace std;

ASTAR::ASTAR( ros::NodeHandle & nh ) {
  nh_ = &nh;
  waypoints_done_ = false;
  initialised_ = false;
  nh_->param<double>( "lambda", lambda_, 1.0 );
  nh_->param<double>( "map_width", map_width_, 6.44 );
  nh_->param<double>( "map_height", map_height_, 3.33 );
  nh_->param<double>( "map_resolution", map_resolution_, 0.01 );
  nh_->param<double>( "grid_resolution", grid_resolution_, 0.06 );
  nh_->param<double>( "startx", start_[0], 0.00 );
  nh_->param<double>( "starty", start_[1], 1.50 );
  nh_->param<double>( "goalx", goal_[0], 2.50 );
  nh_->param<double>( "goaly", goal_[1], 1.50 );

  cout << "setting parameters ... done!\n";
  map_img_ = cv::imread( ros::package::getPath("path_planner")+"/data/map.png", CV_LOAD_IMAGE_GRAYSCALE);
  plan_pub_ = nh.advertise<nav_msgs::Path>("plan", 1);

  boost::thread thread = boost::thread( boost::bind(&ASTAR::path_search, this) );
}


// data type convert functions
double ASTAR::grid2meterX(int x) {
  double nx = x*grid_resolution_-map_width_/2+grid_resolution_/2;
  return nx;
}

double ASTAR::grid2meterY(int y) {
  double ny = map_height_/2-y*grid_resolution_-grid_resolution_/2;
  return ny;
}

int ASTAR::meterX2grid(double x) {
  int gx = round((x+map_width_/2)/grid_resolution_);
  if ( gx > grid_width_-1 )
    gx = grid_width_-1;
  if ( gx < 0 )
    gx = 0;
  return gx;
}

int ASTAR::meterY2grid(double y) {
  int gy = round((map_height_/2-y)/grid_resolution_);
  if ( gy > grid_height_-1 )
    gy = grid_height_-1;
  if ( gy < 0 )
    gy = 0;
  return gy;
}

// print functions
void ASTAR::debug_break()
{
    std::cout << "Enter a letter to continue" << std::endl;
    char ch;
    std::cin.get();
}

void ASTAR::print_list(vector<Node> nodelist) {
  for ( int i = 0; i < (int)nodelist.size(); ++ i ) {
    Node & n = nodelist[i];
    cout << "idx [" << n.x << ", " << n.y << "] with cost = " << n.cost << endl;
  }
}

void ASTAR::print_grid_props(const char *c) {
    if ( strcmp(c, "c") == 0 )
        ROS_INFO("\n\n---------------Grid_property: closed status---------------");
    else if ( strcmp(c, "h") == 0 )
        ROS_INFO("\n\n---------------Grid_property: heuristic cost---------------");
    else if ( strcmp(c, "e") == 0 )
        ROS_INFO("\n\n---------------Grid_property: expansion level---------------");
    else if ( strcmp(c, "o") == 0 )
        ROS_INFO("\n\n---------------Grid_property: occupied status---------------");

  for ( int i = 0; i < grid_height_; ++ i ) {
    for ( int j = 0; j < grid_width_; ++ j ) {
      if ( strcmp(c, "c") == 0 )
        cout << gridmap_[i][j].closed << " ";
      else if ( strcmp(c, "h") == 0 )
        cout << gridmap_[i][j].heuristic << " ";
      else if ( strcmp(c, "e") == 0 )
        cout << gridmap_[i][j].expand << " ";
      else if ( strcmp(c, "o") == 0 )
        cout << gridmap_[i][j].occupied << " ";
    }
    cout << endl;
  }
  cout << endl;
}

void ASTAR::print_waypoints(vector<Waypoint> waypoints) {
  cout << "Waypoints:\n";
  for ( int i = 0; i < (int)waypoints.size(); ++ i ) {
    Waypoint & w = waypoints[i];
    cout << "[" << w.x << ", " << w.y << "]\n";
  }
}



vector<Node> ASTAR::descending_sort( vector<Node> nodelist) {
  Node temp;
  for ( int i = 0; i < (int)nodelist.size(); ++ i ) {
    for ( int j = 0; j < (int)nodelist.size()-1; ++ j ) {
      if ( nodelist[j].cost < nodelist[j+1].cost ) {
        temp = nodelist[j];
        nodelist[j] = nodelist[j+1];
        nodelist[j+1] = temp;
      }
    }
  }
  return nodelist;
}

int ASTAR::contains( const vector<Node>& nodelist, const Node& q_node ){
    // checks if q_node is in nodelist
    // return q_node's index in nodelist if found, -1 otherwise

    vector<Node>::const_iterator match_itor = std::find_if( nodelist.begin(), nodelist.end(),
                                                            [q_node](const Node& n) {
                                                                return n.x == q_node.x && n.y == q_node.y;
                                                            });
    if ( match_itor == nodelist.end( ) )
        return -1;

    return match_itor - nodelist.begin();
}

// initialise grid map
void ASTAR::setup_gridmap(){
  gridmap_.clear();
  grid_height_ = int(ceil(map_height_/grid_resolution_));
  grid_width_  = int(ceil(map_width_/grid_resolution_));

  cout << "grid: size = ( " << grid_width_ << " x " << grid_height_ << " )" << endl;

  for ( int y = 0; y < grid_height_; ++ y ) {
    vector<Grid> gridx;
    for ( int x = 0; x < grid_width_; ++ x ) {
      Grid d;
      d.closed = 0; d.occupied = 0;
      d.heuristic = 0; d.expand = numeric_limits<int>::max();
      gridx.push_back( d );
    }
    gridmap_.push_back( gridx );
  }

  // set occupied flag
  int step = (int)(grid_resolution_/map_resolution_);

  for ( int y = 0; y < grid_height_; ++ y ) {
    for ( int x = 0; x < grid_width_; ++ x ) {
      cv::Point tl, dr;
      tl.x = step*x; tl.y = step*y;
      dr.x = min( map_img_.cols, step*(x+1) );
      dr.y = min( map_img_.rows, step*(y+1) );

      bool found = false;
      for ( int i = tl.y; i < dr.y; ++ i ) {
        for ( int j = tl.x; j < dr.x; ++ j ) {
          if ( (int)map_img_.at<unsigned char>(i,j) != 255 ) {
            found = true;
          }
        }
      }
      gridmap_[y][x].occupied = found == true? 1: 0;
    }
  }
}


// init heuristic
void ASTAR::init_heuristic(Node goal_node) {
  // initialise heuristic value for each grid in the map

/************************ YOUR CODE GOES HERE ************************/

double x1 = goal_node.x;
double y1 = goal_node.y;

for (int y = 0; y < grid_height_; ++y)
{
  for (int x = 0; x < grid_width_; ++x)
  {
    // Grid d; //not sure if we need this

    gridmap_[y][x].heuristic = abs(x1 - x) + abs(y1 - y);
  }
}

print_grid_props("h");
/********************************************************************/

}

// generate policy
void ASTAR::policy(Node start_node, Node goal_node) {
  std::cout << "############################################" << std::endl;
  std::cout << "#         Retrieve optimum policy          #" << std::endl;
  std::cout << "############################################" << std::endl;

  Node current;
  current = goal_node;
  bool found = false;
  optimum_policy_.clear();
  optimum_policy_.push_back( current );

  // search for the minimum cost in the neighbourhood.
  // from goal to start

  /************************ YOUR CODE GOES HERE ************************/
  Node neighbour_node[4];
  Node n;
  int lowest_cost = 0;
while(!found)
{
  if(current.x == start_node.x && current.y == start_node.y)
    found = true;
  else
  {
       lowest_cost = numeric_limits<int>::max();
       neighbour_node[0].x = current.x +1;
       neighbour_node[0].y = current.y;
       neighbour_node[0].cost = gridmap_[neighbour_node[0].y][neighbour_node[0].x].expand;
       neighbour_node[1].x = current.x -1;
       neighbour_node[1].y = current.y;
       neighbour_node[1].cost = gridmap_[neighbour_node[1].y][neighbour_node[1].x].expand;
       neighbour_node[2].x = current.x;
       neighbour_node[2].y = current.y +1;
       neighbour_node[2].cost = gridmap_[neighbour_node[2].y][neighbour_node[2].x].expand;
       neighbour_node[3].x = current.x;
       neighbour_node[3].y = current.y -1;
       neighbour_node[3].cost = gridmap_[neighbour_node[3].y][neighbour_node[3].x].expand;
       int count = 4;
       for (int i = 0; i < count; i++)
       {
         if(gridmap_[neighbour_node[i].y][neighbour_node[i].x].closed = 1)
         {
           //determine lowest cost neighbour only interested if in closed list and not already in list
           if(contains(optimum_policy_, neighbour_node[i]) == -1)
           {
             if(lowest_cost > gridmap_[neighbour_node[i].y][neighbour_node[i].x].expand )
             {
               lowest_cost = gridmap_[neighbour_node[i].y][neighbour_node[i].x].expand;
               current.x = neighbour_node[i].x;
               current.y = neighbour_node[i].y;
               current.cost = gridmap_[neighbour_node[i].y][neighbour_node[i].x].expand;
             }
           }
         }
       }
       optimum_policy_.insert(optimum_policy_.begin(),current);

  }



}




  /********************************************************************/

}

// update waypoints
void ASTAR::update_waypoints(double *robot_pose) {
  waypoints_.clear();
  Waypoint w;
  w.x = robot_pose[0];
  w.y = robot_pose[1];
  waypoints_.push_back( w );
  for ( int i = 1; i < (int)optimum_policy_.size(); ++ i ) {
    w.x = grid2meterX(optimum_policy_[i].x);
    w.y = grid2meterY(optimum_policy_[i].y);
    waypoints_.push_back( w );
  }

  smooth_path( 0.5, 0.3);


  poses_.clear();
  string map_id = "/map";
  ros::Time plan_time = ros::Time::now();
  for ( int i = 0; i < (int)waypoints_.size(); ++ i ) {
    Waypoint & w = waypoints_[i];
    geometry_msgs::PoseStamped p;
    p.header.stamp = plan_time;
    p.header.frame_id = map_id;
    p.pose.position.x = w.x;
    p.pose.position.y = w.y;
    p.pose.position.z = 0.0;
    p.pose.orientation.x = 0.0;
    p.pose.orientation.y = 0.0;
    p.pose.orientation.z = 0.0;
    p.pose.orientation.w = 0.0;
    poses_.push_back( p );
  }
  waypoints_done_ = true;
}

// smooth generated path
void ASTAR::smooth_path(double weight_data, double weight_smooth) {
  double tolerance = 0.001;// 0.00001;

  // smooth paths

/************************ YOUR CODE GOES HERE ************************/
 // initialise smooth waypoints with original waypoints

/* double path_tol;
vector<Waypoint> si;
vector<Waypoint> pi;
waypoints_.pop_back();

for (int i = 1; i < ((int)waypoints_.size()-1); i++) {

  si[i] = waypoints_[i];
  pi[i] = waypoints_[i];

  si[i].x = si[i].x - ((weight_data + (2*weight_smooth))*si[i].x) + (weight_data*pi[i].x) + (weight_smooth*si[i-1].x) + (weight_smooth*si[i+1].x);
  si[i].y = si[i].y - ((weight_data + (2*weight_smooth))*si[i].y) + (weight_data*pi[i].y) + (weight_smooth*si[i-1].y) + (weight_smooth*si[i+1].y);

  path_tol += pow((abs(si[i].x - si[i].x)),2) + pow((abs(si[i].y - si[i].y)),2);

  if (path_tol < tolerance) {
    break;
  }

}


/********************************************************************/

}

// search for astar path planning
bool ASTAR::path_search() {
  setup_gridmap();

  std::cout << "############################################" << std::endl;
  std::cout << "#            initial grid map state        #" << std::endl;
  std::cout << "############################################" << std::endl;
  print_grid_props("o");

  Node start_node, goal_node;
  start_node.x = meterX2grid(start_[0]);
  start_node.y = meterY2grid(start_[1]);
  start_node.cost = 0;
  goal_node.x = meterX2grid( goal_[0] );
  goal_node.y = meterY2grid( goal_[1] );



  // check start of goal is occupied
  if ( gridmap_[start_node.y][start_node.x].occupied != 0 ||
       gridmap_[goal_node.y][goal_node.x].occupied != 0) {
    cout << "The goal/start is occupied\n";
    cout << "Please change another position\n";
    return false;
  }

  init_heuristic(goal_node);

  gridmap_[start_node.y][start_node.x].closed = 1;
  gridmap_[start_node.y][start_node.x].expand = 0;

  // create open list

  /************************ YOUR CODE GOES HERE ************************/

  bool found = false;
  vector<Node> openList;
  Node current_node;
  Node neighbour_node[4];

  double x1 = goal_node.x;
  double y1 = goal_node.y;

  int neighbours_expanded = 0;
  openList.push_back(start_node);
  while (!found)
  {
    print_list(openList);
    openList = descending_sort(openList);
    current_node = openList.back(); //minimum cost node
    openList.pop_back(); //removes last node in vector
    gridmap_[current_node.y][current_node.x].closed = 1;


    if (current_node.x == goal_node.x && current_node.y == goal_node.y) {
      found = true;

    }
    else
    {

      neighbour_node[0].x = current_node.x +1;
      neighbour_node[0].y = current_node.y;
      neighbour_node[0].cost = current_node.cost + 1;
      neighbour_node[1].x = current_node.x -1;
      neighbour_node[1].y = current_node.y;
      neighbour_node[1].cost = current_node.cost + 1;
      neighbour_node[2].x = current_node.x;
      neighbour_node[2].y = current_node.y +1;
      neighbour_node[2].cost = current_node.cost + 1;
      neighbour_node[3].x = current_node.x;
      neighbour_node[3].y = current_node.y -1;
      neighbour_node[3].cost = current_node.cost + 1;
      int count = 4;
      for (int i = 0; i < count; i++)
      {
        neighbour_node[i].cost = neighbour_node[i].cost + (lambda_ * gridmap_[neighbour_node[i].y][neighbour_node[i].x].heuristic);
        if(gridmap_[neighbour_node[i].y][neighbour_node[i].x].occupied == 0)
       {
        if (gridmap_[neighbour_node[i].y][neighbour_node[i].x].closed ==0)
        {
           int found_at = contains(openList, neighbour_node[i]);

           if (found_at == -1)
           {
             // not found in open list
             openList.push_back(neighbour_node[i]);
           }
           else
           {
             Node n = openList.at(found_at);

             if (n.cost > neighbour_node[i].cost)
             {

               n.cost = neighbour_node[i].cost;
             }
           }

           neighbours_expanded = neighbours_expanded + 1;
           gridmap_[neighbour_node[i].y][neighbour_node[i].x].expand = neighbours_expanded;

        }

      }
      }

    }
  }

  print_grid_props("c");





  cout<<"\nupdating policy" <<endl;
  policy( start_node, goal_node );

  cout<<"\nupdating waypoints" <<endl;
  update_waypoints( start_ );

  initialised_ = true;

  publish_plan(poses_);

  return true;
}

void ASTAR::publish_plan(const vector<geometry_msgs::PoseStamped> &path) {
  if ( !initialised_ ) {
    ROS_ERROR("This planner has not been initialized yet, but it is being used, please call initialize() before use");
    return;
  }

  // create a message for the plan
  nav_msgs::Path path_msgs;
  path_msgs.poses.resize( path.size() );
  if (!path.empty()) {
    path_msgs.header.frame_id = path[0].header.frame_id;
    path_msgs.header.stamp = path[0].header.stamp;
  }

  for ( int i = 0; i < (int)path.size(); ++ i ) {
    path_msgs.poses[i] = path[i];
  }
  while (initialised_) {
    plan_pub_.publish( path_msgs );
  }
}





int main( int argc, char ** argv ) {
  ros::init( argc, argv, "path_planner" );
  ros::NodeHandle nh("~");
  ASTAR astar( nh );
  ros::spin();
  return 1;
}
