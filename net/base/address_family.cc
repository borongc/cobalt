// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/address_family.h"

#include "base/notreached.h"
#include "net/base/ip_address.h"
#include "net/base/sys_addrinfo.h"

namespace net {

AddressFamily GetAddressFamily(const IPAddress& address) {
  if (address.IsIPv4()) {
    return ADDRESS_FAMILY_IPV4;
  } else if (address.IsIPv6()) {
    return ADDRESS_FAMILY_IPV6;
  } else {
    return ADDRESS_FAMILY_UNSPECIFIED;
  }
}

#if defined(STARBOARD)
SbSocketAddressType ConvertAddressFamily(AddressFamily address_family) {
  switch (address_family) {
    case ADDRESS_FAMILY_IPV4:
      return kSbSocketAddressTypeIpv4;
      break;
    case ADDRESS_FAMILY_IPV6:
      return kSbSocketAddressTypeIpv6;
      break;
    default:
      NOTREACHED();
      return kSbSocketAddressTypeIpv4;
  }
}
#else
int ConvertAddressFamily(AddressFamily address_family) {
  switch (address_family) {
    case ADDRESS_FAMILY_UNSPECIFIED:
      return AF_UNSPEC;
    case ADDRESS_FAMILY_IPV4:
      return AF_INET;
    case ADDRESS_FAMILY_IPV6:
      return AF_INET6;
  }
  NOTREACHED();
  return AF_UNSPEC;
}
#endif

AddressFamily ToAddressFamily(int family) {
  switch (family) {
    case AF_INET:
      return ADDRESS_FAMILY_IPV4;
    case AF_INET6:
      return ADDRESS_FAMILY_IPV6;
    case AF_UNSPEC:
      return ADDRESS_FAMILY_UNSPECIFIED;
  }
  NOTREACHED();
  return ADDRESS_FAMILY_UNSPECIFIED;
}

}  // namespace net
