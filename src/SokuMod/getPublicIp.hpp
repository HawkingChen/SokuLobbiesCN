//
// Created by PinkySmile on 04/11/2020.
//

#ifndef HISOUTENSOKUDISCORDINTEGRATION_GETPUBLICIP_HPP
#define HISOUTENSOKUDISCORDINTEGRATION_GETPUBLICIP_HPP
#include <atomic>
#include <string>

// Returns an owned snapshot. Call only from a worker: a cache miss performs I/O.
std::string getMyIp(const std::atomic_bool *cancel = nullptr);
std::string getMyIpv6();
bool isIpv6Available();
void setMyIpv6(std::string &ipv6);

#endif // HISOUTENSOKUDISCORDINTEGRATION_GETPUBLICIP_HPP
