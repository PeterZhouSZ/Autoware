/*
 *  Copyright (c) 2015, Nagoya University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name of Autoware nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <arpa/inet.h>
#include <netinet/in.h>

#include <thread>

#include <boost/thread.hpp>

#include <ros/ros.h>
#include <ros/console.h>

#include <geometry_msgs/PoseStamped.h>

#include <tablet_socket/mode_info.h>

#include <udon_socket/udon.hpp>

namespace {

struct Vehicle {
	std::int32_t mode;
	udon_socket::udon::Location location;
};

Vehicle vehicle;
boost::shared_mutex vehicle_mtx;

void cache_mode(const tablet_socket::mode_info& msg)
{
	boost::upgrade_lock<boost::shared_mutex> up_lock(vehicle_mtx);
	boost::upgrade_to_unique_lock<boost::shared_mutex> write_lock(up_lock);

	vehicle.mode = msg.mode;
}

void cache_pose(const geometry_msgs::PoseStamped& msg)
{
	boost::upgrade_lock<boost::shared_mutex> up_lock(vehicle_mtx);
	boost::upgrade_to_unique_lock<boost::shared_mutex> write_lock(up_lock);

	// msg's X-Y axis is reversed
	vehicle.location.x = msg.pose.position.y;
	vehicle.location.y = msg.pose.position.x;
	vehicle.location.z = msg.pose.position.z;
}

void send_info(const sockaddr_in client_addr, int connect_fd, std::size_t bufsize, int period)
{
	std::uint8_t *buf;
	bool first;

	char astr[INET_ADDRSTRLEN];
	if (inet_ntop(AF_INET, &client_addr.sin_addr, astr, sizeof(astr)) == nullptr) {
		ROS_ERROR_STREAM("inet_ntop: " << std::strerror(errno));
		goto close_connect_fd;
	}

	buf = new std::uint8_t[bufsize];
	first = true;

	Vehicle curr, prev;
	while (true) {
		boost::shared_lock<boost::shared_mutex> read_lock(vehicle_mtx);
		curr = vehicle;
		read_lock.unlock();

		if (first) {
			ssize_t nbytes = udon_socket::udon::send_request(connect_fd);
			if (nbytes < 0) {
				ROS_ERROR_STREAM("udon_socket::udon::send_request: " << std::strerror(errno));
				goto delete_buf;
			}
			nbytes = recv(connect_fd, buf, bufsize, 0);
			if (nbytes < 0) {
				ROS_ERROR_STREAM("recv: " << std::strerror(errno));
				goto delete_buf;
			} else if (nbytes == 0) {
				ROS_INFO_STREAM("disconnect " << astr << ":" << ntohs(client_addr.sin_port));
				goto delete_buf;
			}

			nbytes = udon_socket::udon::send_mode(connect_fd, curr.mode);
			if (nbytes < 0) {
				ROS_ERROR_STREAM("udon_socket::udon::send_mode: " << std::strerror(errno));
				goto delete_buf;
			}
			nbytes = recv(connect_fd, buf, bufsize, 0);
			if (nbytes < 0) {
				ROS_ERROR_STREAM("recv: " << std::strerror(errno));
				goto delete_buf;
			} else if (nbytes == 0) {
				ROS_INFO_STREAM("disconnect " << astr << ":" << ntohs(client_addr.sin_port));
				goto delete_buf;
			}

			if (curr.mode == udon_socket::udon::MODE_AUTO) {
				nbytes = udon_socket::udon::send_location(connect_fd, curr.location);
				if (nbytes < 0) {
					ROS_ERROR_STREAM("udon_socket::udon::send_location: " << std::strerror(errno));
					goto delete_buf;
				}
				nbytes = recv(connect_fd, buf, bufsize, 0);
				if (nbytes < 0) {
					ROS_ERROR_STREAM("recv: " << std::strerror(errno));
					goto delete_buf;
				} else if (nbytes == 0) {
					ROS_INFO_STREAM("disconnect " << astr << ":" << ntohs(client_addr.sin_port));
					goto delete_buf;
				}
			}

			first = false;
		} else {
			if (curr.mode != prev.mode) {
				ssize_t nbytes = udon_socket::udon::send_mode(connect_fd, curr.mode);
				if (nbytes < 0) {
					ROS_ERROR_STREAM("udon_socket::udon::send_mode: " << std::strerror(errno));
					goto delete_buf;
				}
				nbytes = recv(connect_fd, buf, bufsize, 0);
				if (nbytes < 0) {
					ROS_ERROR_STREAM("recv: " << std::strerror(errno));
					goto delete_buf;
				} else if (nbytes == 0) {
					ROS_INFO_STREAM("disconnect " << astr << ":" << ntohs(client_addr.sin_port));
					goto delete_buf;
				}
			}

			if (curr.mode == udon_socket::udon::MODE_AUTO && curr.location != prev.location) {
				ssize_t nbytes = udon_socket::udon::send_location(connect_fd, curr.location);
				if (nbytes < 0) {
					ROS_ERROR_STREAM("udon_socket::udon::send_location: " << std::strerror(errno));
					goto delete_buf;
				}
				nbytes = recv(connect_fd, buf, bufsize, 0);
				if (nbytes < 0) {
					ROS_ERROR_STREAM("recv: " << std::strerror(errno));
					goto delete_buf;
				} else if (nbytes == 0) {
					ROS_INFO_STREAM("disconnect " << astr << ":" << ntohs(client_addr.sin_port));
					goto delete_buf;
				}
			}
		}

		prev = curr;

		std::this_thread::sleep_for(std::chrono::milliseconds(period));
	}

delete_buf:
	delete[] buf;
close_connect_fd:
	close(connect_fd);
}

void accept_sock(int backlog, std::size_t bufsize, int period, std::uint16_t port)
{
	int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_fd < 0) {
		ROS_ERROR_STREAM("socket: " << std::strerror(errno));
		return;
	}

	const int on = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
		ROS_ERROR_STREAM("setsockopt: " << std::strerror(errno));
		goto close_listen_fd;
	}

	sockaddr_in server_addr;
	std::memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(listen_fd, reinterpret_cast<const sockaddr *>(&server_addr), sizeof(server_addr)) < 0) {
		ROS_ERROR_STREAM("bind: " << std::strerror(errno));
		goto close_listen_fd;
	}

	if (listen(listen_fd, backlog) < 0) {
		ROS_ERROR_STREAM("listen: " << std::strerror(errno));
		goto close_listen_fd;
	}

	char astr[INET_ADDRSTRLEN];
	if (inet_ntop(AF_INET, &server_addr.sin_addr, astr, sizeof(astr)) == nullptr) {
		ROS_ERROR_STREAM("inet_ntop: " << std::strerror(errno));
		goto close_listen_fd;
	}
	ROS_INFO_STREAM("listen " << astr << ":" << ntohs(server_addr.sin_port));

	int connect_fd;
	while (true) {
		sockaddr_in client_addr;
		socklen_t len = sizeof(client_addr);
		connect_fd = accept(listen_fd, reinterpret_cast<sockaddr *>(&client_addr), &len);
		if (connect_fd < 0) {
			ROS_ERROR_STREAM("accept: " << std::strerror(errno));
			goto close_listen_fd;
		}

		if (inet_ntop(AF_INET, &client_addr.sin_addr, astr, sizeof(astr)) == nullptr) {
			ROS_ERROR_STREAM("inet_ntop: " << std::strerror(errno));
			goto close_connect_fd;
		}
		ROS_INFO_STREAM("connect " << astr << ":" << ntohs(client_addr.sin_port));

		try {
			std::thread sender(send_info, client_addr, connect_fd, bufsize, period);
			sender.detach();
		} catch (std::exception &ex) {
			ROS_ERROR_STREAM("std::thread::thread: " << ex.what());
			goto close_connect_fd;
		}
	}

close_connect_fd:
	close(connect_fd);
close_listen_fd:
	close(listen_fd);
}

} // namespace

int main(int argc, char **argv)
{
	ros::init(argc, argv, "udon_sender");

	ros::NodeHandle n;

	int backlog;
	n.param<int>("/udon_sender/backlog", backlog, 128);
	int bufsize;
	n.param<int>("/udon_sender/bufsize", bufsize, 4096);
	int period;
	n.param<int>("/udon_sender/period", period, 200);
	int port;
	n.param<int>("/udon_sender/port", port, 5999);
	int sub_mode_queue_size;
	n.param<int>("/udon_sender/sub_mode_queue_size", sub_mode_queue_size, 1);
	int sub_pose_queue_size;
	n.param<int>("/udon_sender/sub_pose_queue_size", sub_pose_queue_size, 1);
	ROS_INFO_STREAM("backlog = " << backlog);
	ROS_INFO_STREAM("bufsize = " << bufsize);
	ROS_INFO_STREAM("period = " << period);
	ROS_INFO_STREAM("port = " << port);
	ROS_INFO_STREAM("sub_mode_queue_size = " << sub_mode_queue_size);
	ROS_INFO_STREAM("sub_pose_queue_size = " << sub_pose_queue_size);

	ros::Subscriber mode_sub = n.subscribe("/mode_info", sub_mode_queue_size, cache_mode);
	ros::Subscriber pose_sub = n.subscribe("/current_pose", sub_pose_queue_size, cache_pose);

	try {
		std::thread server(accept_sock, backlog, bufsize, period, port);
		server.detach();
	} catch (std::exception &ex) {
		ROS_ERROR_STREAM("std::thread::thread: " << ex.what());
		return EXIT_FAILURE;
	}

	ros::spin();

	return EXIT_SUCCESS;
}
