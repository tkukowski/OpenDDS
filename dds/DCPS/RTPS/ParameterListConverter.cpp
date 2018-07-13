/*
 *
 *
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include "ParameterListConverter.h"
#include "dds/DCPS/GuidUtils.h"
#include "dds/DCPS/Qos_Helper.h"
#include "dds/DCPS/Service_Participant.h"
#include "dds/DCPS/RTPS/BaseMessageUtils.h"

#include <cstring>

OPENDDS_BEGIN_VERSIONED_NAMESPACE_DECL

namespace OpenDDS {
namespace RTPS {

#ifndef OPENDDS_SAFETY_PROFILE
using DCPS::operator!=;
#endif

namespace {

  void add_param(ParameterList& param_list, const Parameter& param) {
    const CORBA::ULong length = param_list.length();
    param_list.length(length + 1);
    param_list[length] = param;
  }

  void add_param_locator_seq(ParameterList& param_list,
                             const DCPS::LocatorSeq& locator_seq,
                             const ParameterId_t pid) {
    const CORBA::ULong length = locator_seq.length();
    for (CORBA::ULong i = 0; i < length; ++i) {
      Parameter param;
      param.locator(locator_seq[i]);
      param._d(pid);
      add_param(param_list, param);
    }
  }

  void add_param_rtps_locator(ParameterList& param_list,
                              const DCPS::TransportLocator& dcps_locator,
                              bool map /*map IPV4 to IPV6 addr*/) {
    // Convert the tls blob to an RTPS locator seq
    DCPS::LocatorSeq locators;
    bool ignore_requires_inline_qos;
    DDS::ReturnCode_t result = blob_to_locators(dcps_locator.data, locators,
                                                ignore_requires_inline_qos);
    if (result == DDS::RETCODE_OK) {
      const CORBA::ULong locators_len = locators.length();
      for (CORBA::ULong i = 0; i < locators_len; ++i) {
        DCPS::Locator_t& rtps_locator = locators[i];
        ACE_INET_Addr address;
        if (locator_to_address(address, rtps_locator, map) == 0) {
          Parameter param;
          param.locator(rtps_locator);
          if (address.is_multicast()) {
            param._d(PID_MULTICAST_LOCATOR);
          } else {
            param._d(PID_UNICAST_LOCATOR);
          }
          add_param(param_list, param);
        }
      }
    } else {
      ACE_ERROR((LM_ERROR, ACE_TEXT("(%P|%t) ERROR: add_param_rtps_locator - ")
                           ACE_TEXT("Unable to convert dcps_rtps ")
                           ACE_TEXT("TransportLocator blob to LocatorSeq\n")));
    }
  }

  void add_param_dcps_locator(ParameterList& param_list,
                      const DCPS::TransportLocator& dcps_locator) {
    Parameter param;
    param.opendds_locator(dcps_locator);
    param._d(PID_OPENDDS_LOCATOR);
    add_param(param_list, param);
  }

  void append_locator(DCPS::LocatorSeq& list, const DCPS::Locator_t& locator) {
    const CORBA::ULong length = list.length();
    list.length(length + 1);
    list[length] = locator;
  }

  void append_locator(
      DCPS::TransportLocatorSeq& list,
      const DCPS::TransportLocator& locator) {
    const CORBA::ULong length = list.length();
    list.length(length + 1);
    list[length] = locator;
  }

  void append_locators_if_present(
      DCPS::TransportLocatorSeq& list,
      const DCPS::LocatorSeq& rtps_udp_locators) {
    if (rtps_udp_locators.length()) {
      const CORBA::ULong length = list.length();
      list.length(length + 1);
      list[length].transport_type = "rtps_udp";
      locators_to_blob(rtps_udp_locators, list[length].data);
    }
  }
  enum LocatorState {
    locator_undefined,
    locator_complete,
    locator_address_only,
    locator_port_only
  };

  void append_associated_writer(DCPS::DiscoveredReaderData& reader_data,
                                const Parameter& param)
  {
    const CORBA::ULong len = reader_data.readerProxy.associatedWriters.length();
    reader_data.readerProxy.associatedWriters.length(len + 1);
    reader_data.readerProxy.associatedWriters[len] = param.guid();
  }

  void set_ipaddress(DCPS::LocatorSeq& locators,
                     LocatorState& last_state,
                     const unsigned long addr) {
    const CORBA::ULong length = locators.length();
    // Update last locator if the last state is port only
    if (last_state == locator_port_only && length > 0) {
      // Update last locator
      DCPS::Locator_t& partial = locators[length - 1];
      assign(partial.address, addr);
      // there is no longer a partially complete locator, set state
      last_state = locator_complete;
    // Else there is no partially complete locator available
    } else {
      // initialize and append new locator
      DCPS::Locator_t locator;
      locator.kind = LOCATOR_KIND_UDPv4;
      locator.port = 0;
      assign(locator.address, addr);
      locators.length(length + 1);
      locators[length] = locator;
      // there is now a paritally complete locator, set state
      last_state = locator_address_only;
    }
  }

  void set_port(DCPS::LocatorSeq& locators,
                LocatorState& last_state,
                const unsigned long port) {
    const CORBA::ULong length = locators.length();
    // Update last locator if the last state is address only
    if (last_state == locator_address_only && length > 0) {
      // Update last locator
      DCPS::Locator_t& partial = locators[length - 1];
      partial.port = port;
      // there is no longer a partially complete locator, set state
      last_state = locator_complete;
    // Else there is no partially complete locator available
    } else {
      // initialize and append new locator
      DCPS::Locator_t locator;
      locator.kind = LOCATOR_KIND_UDPv4;
      locator.port = port;
      assign(locator.address, 0);
      locators.length(length + 1);
      locators[length] = locator;
      // there is now a paritally complete locator, set state
      last_state = locator_port_only;
    }
  }

  bool not_default(const DDS::UserDataQosPolicy& qos) {
    DDS::UserDataQosPolicy def_qos =
        TheServiceParticipant->initial_UserDataQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::GroupDataQosPolicy& qos) {
    DDS::GroupDataQosPolicy def_qos =
        TheServiceParticipant->initial_GroupDataQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::TopicDataQosPolicy& qos) {
    DDS::TopicDataQosPolicy def_qos =
        TheServiceParticipant->initial_TopicDataQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::DurabilityQosPolicy& qos) {
    DDS::DurabilityQosPolicy def_qos =
        TheServiceParticipant->initial_DurabilityQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::DurabilityServiceQosPolicy& qos) {
    DDS::DurabilityServiceQosPolicy def_qos =
        TheServiceParticipant->initial_DurabilityServiceQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::LifespanQosPolicy& qos) {
    DDS::LifespanQosPolicy def_qos =
        TheServiceParticipant->initial_LifespanQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::DeadlineQosPolicy& qos) {
    DDS::DeadlineQosPolicy def_qos =
        TheServiceParticipant->initial_DeadlineQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::LatencyBudgetQosPolicy& qos) {
    DDS::LatencyBudgetQosPolicy def_qos =
        TheServiceParticipant->initial_LatencyBudgetQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::LivelinessQosPolicy& qos) {
    DDS::LivelinessQosPolicy def_qos =
        TheServiceParticipant->initial_LivelinessQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::OwnershipQosPolicy& qos) {
    DDS::OwnershipQosPolicy def_qos =
        TheServiceParticipant->initial_OwnershipQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::OwnershipStrengthQosPolicy& qos) {
#ifdef OPENDDS_NO_OWNERSHIP_KIND_EXCLUSIVE
    ACE_UNUSED_ARG(qos);
    return false;
#else
    DDS::OwnershipStrengthQosPolicy def_qos =
        TheServiceParticipant->initial_OwnershipStrengthQosPolicy();
    return qos != def_qos;
#endif
  }
  bool not_default(const DDS::DestinationOrderQosPolicy& qos) {
    DDS::DestinationOrderQosPolicy def_qos =
        TheServiceParticipant->initial_DestinationOrderQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::PresentationQosPolicy& qos) {
    DDS::PresentationQosPolicy def_qos =
        TheServiceParticipant->initial_PresentationQosPolicy();
    return qos != def_qos;
  }
  bool not_default(const DDS::PartitionQosPolicy& qos) {
    DDS::PartitionQosPolicy def_qos =
        TheServiceParticipant->initial_PartitionQosPolicy();
    return qos != def_qos;
  }

  bool not_default(const DDS::PropertyQosPolicy& qos) {
    for (unsigned int i = 0; i < qos.value.length(); ++i) {
      if (qos.value[i].propagate) {
        return true;
      }
    }
    // binary_value is not sent in the parameter list (DDSSEC12-37)
    return false;
  }

  bool not_default(const DDS::TimeBasedFilterQosPolicy& qos)
  {
    DDS::TimeBasedFilterQosPolicy def_qos =
      TheServiceParticipant->initial_TimeBasedFilterQosPolicy();
    return qos != def_qos;
  }

  bool not_default(const DCPS::ContentFilterProperty_t& cfprop)
  {
    return std::strlen(cfprop.filterExpression);
  }

  void normalize(DDS::Duration_t& dur)
  {
    // Interoperability note:
    // Some other DDS implementations were observed sending
    // "infinite" durations using 0xffffffff nanoseconds
    if (dur.sec == DDS::DURATION_INFINITE_SEC &&
        dur.nanosec == DDS::TIME_INVALID_NSEC) {
      dur.nanosec = DDS::DURATION_INFINITE_NSEC;
    }
  }
};

namespace ParameterListConverter {

int to_param_list(const SPDPdiscoveredParticipantData& participant_data,
                  ParameterList& param_list)
{
  // Parameterize ParticipantBuiltinTopicData
  // Ignore participant builtin topic key

  if (not_default(participant_data.ddsParticipantData.user_data))
  {
    Parameter ud_param;
    ud_param.user_data(participant_data.ddsParticipantData.user_data);
    add_param(param_list, ud_param);
  }

  // Parameterize ParticipantProxy_t
  Parameter pv_param;
  pv_param.version(participant_data.participantProxy.protocolVersion);
  add_param(param_list, pv_param);

  // For guid prefix, copy into guid, and force some values
  Parameter gp_param;
  GUID_t guid;
  ACE_OS::memcpy(guid.guidPrefix,
                 participant_data.participantProxy.guidPrefix,
                 sizeof(guid.guidPrefix));
  guid.entityId = DCPS::ENTITYID_PARTICIPANT;

  gp_param.guid(guid);
  gp_param._d(PID_PARTICIPANT_GUID);
  add_param(param_list, gp_param);

  Parameter vid_param;
  vid_param.vendor(participant_data.participantProxy.vendorId);
  add_param(param_list, vid_param);

  if (participant_data.participantProxy.expectsInlineQos != false)
  {
    Parameter eiq_param; // Default is false
    eiq_param.expects_inline_qos(
        participant_data.participantProxy.expectsInlineQos);
    add_param(param_list, eiq_param);
  }

  Parameter abe_param;
  abe_param.participant_builtin_endpoints(
    participant_data.participantProxy.availableBuiltinEndpoints);
  add_param(param_list, abe_param);

  // Interoperability note:
  // For interoperability with other DDS implemenations, we'll encode the
  // availableBuiltinEndpoints as PID_BUILTIN_ENDPOINT_SET in addition to
  // PID_PARTICIPANT_BUILTIN_ENDPOINTS (above).
  Parameter be_param;
  be_param.builtin_endpoints(
    participant_data.participantProxy.availableBuiltinEndpoints);
  add_param(param_list, be_param);

  // Each locator
  add_param_locator_seq(
      param_list,
      participant_data.participantProxy.metatrafficUnicastLocatorList,
      PID_METATRAFFIC_UNICAST_LOCATOR);

  add_param_locator_seq(
      param_list,
      participant_data.participantProxy.metatrafficMulticastLocatorList,
      PID_METATRAFFIC_MULTICAST_LOCATOR);

  add_param_locator_seq(
      param_list,
      participant_data.participantProxy.defaultUnicastLocatorList,
      PID_DEFAULT_UNICAST_LOCATOR);

  add_param_locator_seq(
      param_list,
      participant_data.participantProxy.defaultMulticastLocatorList,
      PID_DEFAULT_MULTICAST_LOCATOR);

  Parameter ml_param;
  ml_param.count(participant_data.participantProxy.manualLivelinessCount);
  add_param(param_list, ml_param);

  if ((participant_data.leaseDuration.seconds != 100) ||
      (participant_data.leaseDuration.fraction != 0))
  {
    Parameter ld_param;
    ld_param.duration(participant_data.leaseDuration);
    add_param(param_list, ld_param);
  }

  return 0;
}

int to_param_list(const DDS::ParticipantBuiltinTopicData& pbtd,
                  ParameterList& param_list)
{
  if (not_default(pbtd.user_data))
  {
    Parameter param_ud;
    param_ud.user_data(pbtd.user_data);
    add_param(param_list, param_ud);
  }

  return 0;
}

int to_param_list(const DDS::Security::ParticipantBuiltinTopicData& pbtd,
                  const DCPS::GUID_t& guid,
                  ParameterList& param_list)
{
  Parameter gp_param;
  gp_param.guid(guid);
  gp_param._d(PID_PARTICIPANT_GUID);
  add_param(param_list, gp_param);

  to_param_list(pbtd.base, param_list);

  Parameter param_it;
  param_it.identity_token(pbtd.identity_token);
  add_param(param_list, param_it);

  Parameter param_pt;
  param_pt.permissions_token(pbtd.permissions_token);
  add_param(param_list, param_pt);

  if (not_default(pbtd.property))
  {
    Parameter param_p;
    param_p.property(pbtd.property);
    add_param(param_list, param_p);
  }

  Parameter param_psi;
  param_psi.participant_security_info(pbtd.security_info);
  add_param(param_list, param_psi);

  return 0;
}

int to_param_list(const DDS::Security::IdentityToken& identity_token,
                  const DDS::Security::PermissionsToken& permissions_token,
                  const DDS::PropertyQosPolicy& property_qos,
                  const DDS::Security::ParticipantSecurityInfo& security_info,
                  ParameterList& param_list)
{
  Parameter param_it;
  param_it.identity_token(identity_token);
  add_param(param_list, param_it);

  Parameter param_pt;
  param_pt.permissions_token(permissions_token);
  add_param(param_list, param_pt);

  if (not_default(property_qos))
  {
    Parameter param_p;
    // binary_value is not sent in the parameter list (DDSSEC12-37)
    DDS::PropertyQosPolicy policy_serialized(property_qos);
    policy_serialized.binary_value.length(0);
    param_p.property(policy_serialized);
    add_param(param_list, param_p);
  }

  Parameter param_psi;
  param_psi.participant_security_info(security_info);
  add_param(param_list, param_psi);

  return 0;
}

int to_param_list(const DCPS::DiscoveredWriterData& writer_data,
                  ParameterList& param_list,
                  bool map)
{
  // Ignore builtin topic key

  {
    Parameter param;
    param.string_data(writer_data.ddsPublicationData.topic_name);
    param._d(PID_TOPIC_NAME);
    add_param(param_list, param);
  }
  {
    Parameter param;
    param.string_data(writer_data.ddsPublicationData.type_name);
    param._d(PID_TYPE_NAME);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.durability))
  {
    Parameter param;
    param.durability(writer_data.ddsPublicationData.durability);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.durability_service))
  {
    Parameter param;
    param.durability_service(writer_data.ddsPublicationData.durability_service);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.deadline))
  {
    Parameter param;
    param.deadline(writer_data.ddsPublicationData.deadline);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.latency_budget))
  {
    Parameter param;
    param.latency_budget(writer_data.ddsPublicationData.latency_budget);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.liveliness))
  {
    Parameter param;
    param.liveliness(writer_data.ddsPublicationData.liveliness);
    add_param(param_list, param);
  }

  // Interoperability note:
  // For interoperability, always write the reliability info
  {
    Parameter param;
    // Interoperability note:
    // Spec creators for RTPS have reliability indexed at 1
    DDS::ReliabilityQosPolicy reliability_copy =
        writer_data.ddsPublicationData.reliability;
    reliability_copy.kind =
        (DDS::ReliabilityQosPolicyKind)((int)reliability_copy.kind + 1);
    param.reliability(reliability_copy);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.lifespan))
  {
    Parameter param;
    param.lifespan(writer_data.ddsPublicationData.lifespan);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.user_data))
  {
    Parameter param;
    param.user_data(writer_data.ddsPublicationData.user_data);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.ownership))
  {
    Parameter param;
    param.ownership(writer_data.ddsPublicationData.ownership);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.ownership_strength))
  {
    Parameter param;
    param.ownership_strength(writer_data.ddsPublicationData.ownership_strength);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.destination_order))
  {
    Parameter param;
    param.destination_order(writer_data.ddsPublicationData.destination_order);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.presentation))
  {
    Parameter param;
    param.presentation(writer_data.ddsPublicationData.presentation);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.partition))
  {
    Parameter param;
    param.partition(writer_data.ddsPublicationData.partition);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.topic_data))
  {
    Parameter param;
    param.topic_data(writer_data.ddsPublicationData.topic_data);
    add_param(param_list, param);
  }

  if (not_default(writer_data.ddsPublicationData.group_data))
  {
    Parameter param;
    param.group_data(writer_data.ddsPublicationData.group_data);
    add_param(param_list, param);
  }

  {
    Parameter param;
    param.guid(writer_data.writerProxy.remoteWriterGuid);
    param._d(PID_ENDPOINT_GUID);
    add_param(param_list, param);
  }
  CORBA::ULong locator_len = writer_data.writerProxy.allLocators.length();

  // Serialize from allLocators, rather than the unicastLocatorList
  // and multicastLocatorList.  This allows OpenDDS transports to be
  // serialized in the proper order using custom PIDs.
  for (CORBA::ULong i = 0; i < locator_len; ++i) {
    // Each locator has a blob of interest
    const DCPS::TransportLocator& tl = writer_data.writerProxy.allLocators[i];
    // If this is an rtps udp transport
    if (!std::strcmp(tl.transport_type, "rtps_udp")) {
      // Append the locator's deserialized locator and an RTPS PID
      add_param_rtps_locator(param_list, tl, map);
    // Otherwise, this is an OpenDDS, custom transport
    } else {
      // Append the blob and a custom PID
      add_param_dcps_locator(param_list, tl);
      if (!std::strcmp(tl.transport_type, "multicast")) {
        ACE_DEBUG((LM_WARNING,
                   ACE_TEXT("(%P|%t) to_param_list(dwd) - ")
                   ACE_TEXT("Multicast transport with RTPS ")
                   ACE_TEXT("discovery has known issues")));
      }
    }
  }

  return 0;
}

// Local security-related helper functions.
namespace {

  inline void to_param_list(const DDS::Security::EndpointSecurityInfo& src, ParameterList& dest)
  {
    Parameter param;
    param.endpoint_security_info(src);
    add_param(dest, param);
  }

  inline void to_param_list(const DDS::Security::DataTags& src, ParameterList& dest)
  {
    Parameter param;
    param.data_tags(src);
    add_param(dest, param);
  }

  inline void to_param_list(const DDS::Security::ParticipantSecurityInfo& src, ParameterList& dest)
  {
    Parameter param;
    param.participant_security_info(src);
    add_param(dest, param);
  }

  int to_param_list(const DDS::Security::Token& src,
                    CORBA::UShort pid,
                    ParameterList& dest)
  {
    int result = 0;

    Parameter param;

    switch (pid) {
    case DDS::Security::PID_IDENTITY_STATUS_TOKEN:
      param.identity_status_token(src);
      add_param(dest, param);
      break;

    case DDS::Security::PID_IDENTITY_TOKEN:
      param.identity_token(src);
      add_param(dest, param);
      break;

    case DDS::Security::PID_PERMISSIONS_TOKEN:
      param.permissions_token(src);
      add_param(dest, param);
      break;

    default:
      result = -1;
      break;
    }

    return result;
  }

  int from_param_list(const ParameterList& param_list,
                      DDS::Security::EndpointSecurityInfo& security_info,
                      DDS::Security::DataTags& data_tags)
  {

    data_tags.tags.length(0);
    security_info.endpoint_security_attributes = 0;
    security_info.plugin_endpoint_security_attributes = 0;

    size_t len = param_list.length();
    for (size_t i = 0; i < len; ++i) {

      const Parameter& p = param_list[i];

      switch(p._d()) {
      case DDS::Security::PID_DATA_TAGS:
        data_tags = p.data_tags();
        break;

      case DDS::Security::PID_ENDPOINT_SECURITY_INFO:
        security_info = p.endpoint_security_info();
        break;

      default:
        if (p._d() & PIDMASK_INCOMPATIBLE) {
            return -1;
        }
      }
    }

    return 0;
  }

} /* local security-related helpers */

int to_param_list(const Security::SPDPdiscoveredParticipantData_SecurityWrapper& wrapper,
                    ParameterList& param_list)
{
  int err = to_param_list(wrapper.data, param_list) ||
            to_param_list(wrapper.identity_token, DDS::Security::PID_IDENTITY_TOKEN, param_list) ||
            to_param_list(wrapper.permissions_token, DDS::Security::PID_PERMISSIONS_TOKEN, param_list) ||
            to_param_list(wrapper.identity_status_token, DDS::Security::PID_IDENTITY_STATUS_TOKEN, param_list);

  if (! err) {
      to_param_list(wrapper.security_info, param_list);
  }

  return err;
}

int from_param_list(const ParameterList& param_list,
                      Security::SPDPdiscoveredParticipantData_SecurityWrapper& wrapper)
{
  int result = from_param_list(param_list, wrapper.data) ||
               from_param_list(param_list,
                               wrapper.identity_token,
                               wrapper.permissions_token,
                               wrapper.property_qos,
                               wrapper.security_info,
                               wrapper.identity_status_token);
  return result;
}

int from_param_list(const ParameterList& param_list,
                    DDS::Security::IdentityToken& id_token,
                    DDS::Security::PermissionsToken& perm_token,
                    DDS::PropertyQosPolicy& property_qos,
                    DDS::Security::ParticipantSecurityInfo& security_info)
{
  id_token.class_id = "";
  id_token.properties.length(0);
  id_token.binary_properties.length(0);

  perm_token.class_id = "";
  perm_token.properties.length(0);
  perm_token.binary_properties.length(0);

  property_qos.value.length(0);
  property_qos.binary_value.length(0);

  security_info.participant_security_attributes = 0;
  security_info.plugin_participant_security_attributes = 0;

  unsigned char fieldmask = 0x00;

  enum FieldMaskNames {
    ID_TOKEN_FIELD = 0x01,
    PERM_TOKEN_FIELD = 0x02,
    PROPERTY_LIST_FIELD = 0x04,
    PARTICIPANT_SECURITY_INFO_FIELD = 0x08
  };

  const unsigned char REQUIRED_FIELDS = ID_TOKEN_FIELD | PERM_TOKEN_FIELD;

  size_t len = param_list.length();
  for (size_t i = 0; i < len; ++i) {
    const Parameter& p = param_list[i];

    switch (p._d()) {
    case DDS::Security::PID_IDENTITY_TOKEN:
      id_token = p.identity_token();
      fieldmask |= ID_TOKEN_FIELD;
      break;

    case DDS::Security::PID_PERMISSIONS_TOKEN:
      perm_token = p.permissions_token();
      fieldmask |= PERM_TOKEN_FIELD;
      break;

    case PID_PROPERTY_LIST:
      property_qos = p.property();
      fieldmask |= PROPERTY_LIST_FIELD;
      break;

    case DDS::Security::PID_PARTICIPANT_SECURITY_INFO:
      security_info = p.participant_security_info();
      fieldmask |= PARTICIPANT_SECURITY_INFO_FIELD;
      break;

    default:
      :if (p._d() & PIDMASK_INCOMPATIBLE) {
        return -1;
      }
    }
  }

  return ((fieldmask & REQUIRED_FIELDS) == REQUIRED_FIELDS) ? 0 : -1;
}

int from_param_list(const ParameterList& param_list,
                    DDS::Security::IdentityToken& id_token,
                    DDS::Security::PermissionsToken& perm_token,
                    DDS::PropertyQosPolicy& property_qos,
                    DDS::Security::ParticipantSecurityInfo& security_info,
                    DDS::Security::IdentityStatusToken& id_status_token)
{

  id_token.class_id = "";
  id_token.properties.length(0);
  id_token.binary_properties.length(0);

  perm_token.class_id = "";
  perm_token.properties.length(0);
  perm_token.binary_properties.length(0);

  property_qos.value.length(0);
  property_qos.binary_value.length(0);

  security_info.participant_security_attributes = 0;
  security_info.plugin_participant_security_attributes = 0;

  id_status_token.class_id = "";
  id_status_token.properties.length(0);
  id_status_token.binary_properties.length(0);

  unsigned char fieldmask = 0x00;

  enum FieldMaskNames {
    ID_TOKEN_FIELD = 0x01,
    PERM_TOKEN_FIELD = 0x02,
    PROPERTY_LIST_FIELD = 0x04,
    PARTICIPANT_SECURITY_INFO_FIELD = 0x08,
    IDENTITY_STATUS_TOKEN_FIELD = 0x10
  };

  const unsigned char REQUIRED_FIELDS = ID_TOKEN_FIELD | PERM_TOKEN_FIELD | PARTICIPANT_SECURITY_INFO_FIELD;

  size_t len = param_list.length();
  for (size_t i = 0; i < len; ++i) {
      const Parameter& p = param_list[i];

      switch (p._d()) {
        case DDS::Security::PID_IDENTITY_TOKEN:
          id_token = p.identity_token();
          fieldmask |= ID_TOKEN_FIELD;
          break;

        case DDS::Security::PID_PERMISSIONS_TOKEN:
          perm_token = p.permissions_token();
          fieldmask |= PERM_TOKEN_FIELD;
          break;

        case PID_PROPERTY_LIST:
          property_qos = p.property();
          fieldmask |= PROPERTY_LIST_FIELD;
          break;

        case DDS::Security::PID_PARTICIPANT_SECURITY_INFO:
          security_info = p.participant_security_info();
          fieldmask |= PARTICIPANT_SECURITY_INFO_FIELD;
          break;

        case DDS::Security::PID_IDENTITY_STATUS_TOKEN:
          id_status_token = p.identity_status_token();
          fieldmask |= IDENTITY_STATUS_TOKEN_FIELD;
          break;

        default:
          if (p._d() & PIDMASK_INCOMPATIBLE) {
              return -1;
          }
      }
  }

  return ((fieldmask & REQUIRED_FIELDS) == REQUIRED_FIELDS) ? 0 : -1;
}

int to_param_list(const Security::DiscoveredWriterData_SecurityWrapper& wrapper,
                  ParameterList& param_list,
                  bool map)
{
  int result = to_param_list(wrapper.data, param_list, map);

  to_param_list(wrapper.security_info, param_list);
  to_param_list(wrapper.data_tags, param_list);

  return result;
}

int to_param_list(const DCPS::DiscoveredReaderData& reader_data,
                  ParameterList& param_list,
                  bool map)
{
  // Ignore builtin topic key
  {
    Parameter param;
    param.string_data(reader_data.ddsSubscriptionData.topic_name);
    param._d(PID_TOPIC_NAME);
    add_param(param_list, param);
  }
  {
    Parameter param;
    param.string_data(reader_data.ddsSubscriptionData.type_name);
    param._d(PID_TYPE_NAME);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.durability))
  {
    Parameter param;
    param.durability(reader_data.ddsSubscriptionData.durability);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.deadline))
  {
    Parameter param;
    param.deadline(reader_data.ddsSubscriptionData.deadline);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.latency_budget))
  {
    Parameter param;
    param.latency_budget(reader_data.ddsSubscriptionData.latency_budget);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.liveliness))
  {
    Parameter param;
    param.liveliness(reader_data.ddsSubscriptionData.liveliness);
    add_param(param_list, param);
  }

  // Interoperability note:
  // For interoperability, always write the reliability info
  // if (not_default(reader_data.ddsSubscriptionData.reliability, false))
  {
    Parameter param;
    // Interoperability note:
    // Spec creators for RTPS have reliability indexed at 1
    DDS::ReliabilityQosPolicy reliability_copy =
        reader_data.ddsSubscriptionData.reliability;
    reliability_copy.kind =
        (DDS::ReliabilityQosPolicyKind)((int)reliability_copy.kind + 1);
    param.reliability(reliability_copy);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.user_data))
  {
    Parameter param;
    param.user_data(reader_data.ddsSubscriptionData.user_data);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.ownership))
  {
    Parameter param;
    param.ownership(reader_data.ddsSubscriptionData.ownership);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.destination_order))
  {
    Parameter param;
    param.destination_order(reader_data.ddsSubscriptionData.destination_order);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.time_based_filter))
  {
    Parameter param;
    param.time_based_filter(reader_data.ddsSubscriptionData.time_based_filter);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.presentation))
  {
    Parameter param;
    param.presentation(reader_data.ddsSubscriptionData.presentation);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.partition))
  {
    Parameter param;
    param.partition(reader_data.ddsSubscriptionData.partition);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.topic_data))
  {
    Parameter param;
    param.topic_data(reader_data.ddsSubscriptionData.topic_data);
    add_param(param_list, param);
  }

  if (not_default(reader_data.ddsSubscriptionData.group_data))
  {
    Parameter param;
    param.group_data(reader_data.ddsSubscriptionData.group_data);
    add_param(param_list, param);
  }

  {
    Parameter param;
    param.guid(reader_data.readerProxy.remoteReaderGuid);
    param._d(PID_ENDPOINT_GUID);
    add_param(param_list, param);
  }

  if (not_default(reader_data.contentFilterProperty))
  {
    Parameter param;
    DCPS::ContentFilterProperty_t cfprop_copy = reader_data.contentFilterProperty;
    if (!std::strlen(cfprop_copy.filterClassName)) {
      cfprop_copy.filterClassName = "DDSSQL";
    }
    param.content_filter_property(cfprop_copy);
    add_param(param_list, param);
  }

  CORBA::ULong i;
  CORBA::ULong locator_len = reader_data.readerProxy.allLocators.length();
  // Serialize from allLocators, rather than the unicastLocatorList
  // and multicastLocatorList.  This allows OpenDDS transports to be
  // serialized in the proper order using custom PIDs.
  for (i = 0; i < locator_len; ++i) {
    // Each locator has a blob of interest
    const DCPS::TransportLocator& tl = reader_data.readerProxy.allLocators[i];
    // If this is an rtps udp transport
    if (!std::strcmp(tl.transport_type, "rtps_udp")) {
      // Append the locator's deserialized locator and an RTPS PID
      add_param_rtps_locator(param_list, tl, map);
    // Otherwise, this is an OpenDDS, custom transport
    } else {
      // Append the blob and a custom PID
      add_param_dcps_locator(param_list, tl);
      if (!std::strcmp(tl.transport_type, "multicast")) {
        ACE_DEBUG((LM_WARNING,
                   ACE_TEXT("(%P|%t) to_param_list(drd) - ")
                   ACE_TEXT("Multicast transport with RTPS ")
                   ACE_TEXT("discovery has known issues")));
      }
    }
  }

  CORBA::ULong num_associations =
      reader_data.readerProxy.associatedWriters.length();
  for (i = 0; i < num_associations; ++i) {
    Parameter param;
    param.guid(reader_data.readerProxy.associatedWriters[i]);
    param._d(PID_OPENDDS_ASSOCIATED_WRITER);
    add_param(param_list, param);
  }
  return 0;
}

int to_param_list(const Security::DiscoveredReaderData_SecurityWrapper& wrapper,
                  ParameterList& param_list,
                  bool map)
{
  int result = to_param_list(wrapper.data, param_list, map);

  to_param_list(wrapper.security_info, param_list);
  to_param_list(wrapper.data_tags, param_list);

  return result;
}

int from_param_list(const ParameterList& param_list,
                    SPDPdiscoveredParticipantData& participant_data)
{
  // Track the state of our locators
  LocatorState du_last_state = locator_undefined;
  LocatorState mu_last_state = locator_undefined;
  LocatorState mm_last_state = locator_undefined;

  // Start by setting defaults
  participant_data.ddsParticipantData.user_data.value.length(0);
  participant_data.participantProxy.availableBuiltinEndpoints = 0;
  participant_data.participantProxy.expectsInlineQos = false;
  participant_data.leaseDuration.seconds = 100;
  participant_data.leaseDuration.fraction = 0;

  CORBA::ULong length = param_list.length();
  for (CORBA::ULong i = 0; i < length; ++i) {
    const Parameter& param = param_list[i];
    switch (param._d()) {
      case PID_USER_DATA:
        participant_data.ddsParticipantData.user_data = param.user_data();
        break;
      case PID_PROTOCOL_VERSION:
        participant_data.participantProxy.protocolVersion = param.version();
        break;
      case PID_PARTICIPANT_GUID:
        ACE_OS::memcpy(participant_data.participantProxy.guidPrefix,
               param.guid().guidPrefix, sizeof(GuidPrefix_t));
        break;
      case PID_VENDORID:
        ACE_OS::memcpy(participant_data.participantProxy.vendorId.vendorId,
               param.vendor().vendorId, sizeof(OctetArray2));
        break;
      case PID_EXPECTS_INLINE_QOS:
        participant_data.participantProxy.expectsInlineQos =
            param.expects_inline_qos();
        break;
      case PID_PARTICIPANT_BUILTIN_ENDPOINTS:
        participant_data.participantProxy.availableBuiltinEndpoints =
            param.participant_builtin_endpoints();
        break;
      case PID_BUILTIN_ENDPOINT_SET:
        // Interoperability note:
        // OpenSplice uses this in place of PID_PARTICIPANT_BUILTIN_ENDPOINTS
        // Table 9.13 indicates that PID_PARTICIPANT_BUILTIN_ENDPOINTS should be
        // used to represent ParticipantProxy::availableBuiltinEndpoints
        participant_data.participantProxy.availableBuiltinEndpoints =
            param.builtin_endpoints();
        break;
      case PID_METATRAFFIC_UNICAST_LOCATOR:
        append_locator(
            participant_data.participantProxy.metatrafficUnicastLocatorList,
            param.locator());
        break;
      case PID_METATRAFFIC_MULTICAST_LOCATOR:
        append_locator(
            participant_data.participantProxy.metatrafficMulticastLocatorList,
            param.locator());
        break;
      case PID_DEFAULT_UNICAST_LOCATOR:
        append_locator(
            participant_data.participantProxy.defaultUnicastLocatorList,
            param.locator());
        break;
      case PID_DEFAULT_MULTICAST_LOCATOR:
        append_locator(
            participant_data.participantProxy.defaultMulticastLocatorList,
            param.locator());
        break;
      case PID_PARTICIPANT_MANUAL_LIVELINESS_COUNT:
        participant_data.participantProxy.manualLivelinessCount.value =
            param.count().value;
        break;
      case PID_PARTICIPANT_LEASE_DURATION:
        participant_data.leaseDuration = param.duration();
        break;
      case PID_DEFAULT_UNICAST_IPADDRESS:
        set_ipaddress(
          participant_data.participantProxy.defaultUnicastLocatorList,
          du_last_state,
          param.ipv4_address());
        break;
      case PID_METATRAFFIC_UNICAST_IPADDRESS:
        set_ipaddress(
          participant_data.participantProxy.metatrafficUnicastLocatorList,
          mu_last_state,
          param.ipv4_address());
        break;
      case PID_METATRAFFIC_MULTICAST_IPADDRESS:
        set_ipaddress(
          participant_data.participantProxy.metatrafficMulticastLocatorList,
          mm_last_state,
          param.ipv4_address());
        break;
      case PID_DEFAULT_UNICAST_PORT:
        set_port(participant_data.participantProxy.defaultUnicastLocatorList,
                 du_last_state,
                 param.udpv4_port());
        break;
      case PID_METATRAFFIC_UNICAST_PORT:
        set_port(participant_data.participantProxy.metatrafficUnicastLocatorList,
                 mu_last_state,
                 param.udpv4_port());
        break;
      case PID_METATRAFFIC_MULTICAST_PORT:
        set_port(participant_data.participantProxy.metatrafficMulticastLocatorList,
                 mm_last_state,
                 param.udpv4_port());
        break;
      case PID_SENTINEL:
      case PID_PAD:
        // ignore
        break;
      default:
        if (param._d() & PIDMASK_INCOMPATIBLE) {
          return -1;
        }
    }
  }
  return 0;
}

int from_param_list(const ParameterList& param_list,
                    DCPS::DiscoveredWriterData& writer_data)
{
  LocatorState last_state = locator_undefined;  // Track state of locator
  // Collect the rtps_udp locators before appending them to allLocators
  DCPS::LocatorSeq rtps_udp_locators;

  // Start by setting defaults
  writer_data.ddsPublicationData.topic_name = "";
  writer_data.ddsPublicationData.type_name  = "";
  writer_data.ddsPublicationData.durability =
      TheServiceParticipant->initial_DurabilityQosPolicy();
  writer_data.ddsPublicationData.durability_service =
      TheServiceParticipant->initial_DurabilityServiceQosPolicy();
  writer_data.ddsPublicationData.deadline =
      TheServiceParticipant->initial_DeadlineQosPolicy();
  writer_data.ddsPublicationData.latency_budget =
      TheServiceParticipant->initial_LatencyBudgetQosPolicy();
  writer_data.ddsPublicationData.liveliness =
      TheServiceParticipant->initial_LivelinessQosPolicy();
  writer_data.ddsPublicationData.reliability =
      TheServiceParticipant->initial_DataWriterQos().reliability;
  writer_data.ddsPublicationData.lifespan =
      TheServiceParticipant->initial_LifespanQosPolicy();
  writer_data.ddsPublicationData.user_data =
      TheServiceParticipant->initial_UserDataQosPolicy();
  writer_data.ddsPublicationData.ownership =
      TheServiceParticipant->initial_OwnershipQosPolicy();
#ifdef OPENDDS_NO_OWNERSHIP_KIND_EXCLUSIVE
  writer_data.ddsPublicationData.ownership_strength.value = 0;
#else
  writer_data.ddsPublicationData.ownership_strength =
      TheServiceParticipant->initial_OwnershipStrengthQosPolicy();
#endif
  writer_data.ddsPublicationData.destination_order =
      TheServiceParticipant->initial_DestinationOrderQosPolicy();
  writer_data.ddsPublicationData.presentation =
      TheServiceParticipant->initial_PresentationQosPolicy();
  writer_data.ddsPublicationData.partition =
      TheServiceParticipant->initial_PartitionQosPolicy();
  writer_data.ddsPublicationData.topic_data =
      TheServiceParticipant->initial_TopicDataQosPolicy();
  writer_data.ddsPublicationData.group_data =
      TheServiceParticipant->initial_GroupDataQosPolicy();
  writer_data.writerProxy.unicastLocatorList.length(0);
  writer_data.writerProxy.multicastLocatorList.length(0);

  CORBA::ULong length = param_list.length();
  for (CORBA::ULong i = 0; i < length; ++i) {
    const Parameter& param = param_list[i];
    switch (param._d()) {
      case PID_TOPIC_NAME:
        writer_data.ddsPublicationData.topic_name = param.string_data();
        break;
      case PID_TYPE_NAME:
        writer_data.ddsPublicationData.type_name = param.string_data();
        break;
      case PID_DURABILITY:
        writer_data.ddsPublicationData.durability = param.durability();
        break;
      case PID_DURABILITY_SERVICE:
        writer_data.ddsPublicationData.durability_service =
             param.durability_service();
        // Interoperability note: calling normalize() shouldn't be required
        normalize(writer_data.ddsPublicationData.durability_service.service_cleanup_delay);
        break;
      case PID_DEADLINE:
        writer_data.ddsPublicationData.deadline = param.deadline();
        // Interoperability note: calling normalize() shouldn't be required
        normalize(writer_data.ddsPublicationData.deadline.period);
        break;
      case PID_LATENCY_BUDGET:
        writer_data.ddsPublicationData.latency_budget = param.latency_budget();
        // Interoperability note: calling normalize() shouldn't be required
        normalize(writer_data.ddsPublicationData.latency_budget.duration);
        break;
      case PID_LIVELINESS:
        writer_data.ddsPublicationData.liveliness = param.liveliness();
        // Interoperability note: calling normalize() shouldn't be required
        normalize(writer_data.ddsPublicationData.liveliness.lease_duration);
        break;
      case PID_RELIABILITY:
        writer_data.ddsPublicationData.reliability = param.reliability();
        // Interoperability note:
        // Spec creators for RTPS have reliability indexed at 1
        writer_data.ddsPublicationData.reliability.kind =
          (DDS::ReliabilityQosPolicyKind)
              ((int)writer_data.ddsPublicationData.reliability.kind - 1);
        normalize(writer_data.ddsPublicationData.reliability.max_blocking_time);
        break;
      case PID_LIFESPAN:
        writer_data.ddsPublicationData.lifespan = param.lifespan();
        // Interoperability note: calling normalize() shouldn't be required
        normalize(writer_data.ddsPublicationData.lifespan.duration);
        break;
      case PID_USER_DATA:
        writer_data.ddsPublicationData.user_data = param.user_data();
        break;
      case PID_OWNERSHIP:
        writer_data.ddsPublicationData.ownership = param.ownership();
        break;
      case PID_OWNERSHIP_STRENGTH:
        writer_data.ddsPublicationData.ownership_strength = param.ownership_strength();
        break;
      case PID_DESTINATION_ORDER:
        writer_data.ddsPublicationData.destination_order = param.destination_order();
        break;
      case PID_PRESENTATION:
        writer_data.ddsPublicationData.presentation = param.presentation();
        break;
      case PID_PARTITION:
        writer_data.ddsPublicationData.partition = param.partition();
        break;
      case PID_TOPIC_DATA:
        writer_data.ddsPublicationData.topic_data = param.topic_data();
        break;
      case PID_GROUP_DATA:
        writer_data.ddsPublicationData.group_data = param.group_data();
        break;
      case PID_ENDPOINT_GUID:
        writer_data.writerProxy.remoteWriterGuid = param.guid();
        break;
      case PID_UNICAST_LOCATOR:
        append_locator(rtps_udp_locators, param.locator());
        break;
      case PID_MULTICAST_LOCATOR:
        append_locator(rtps_udp_locators, param.locator());
        break;
      case PID_MULTICAST_IPADDRESS:
        set_ipaddress(rtps_udp_locators,
                      last_state,
                      param.ipv4_address());
        break;
      case PID_OPENDDS_LOCATOR:
        // Append the rtps_udp_locators, if any, first, to preserve order
        append_locators_if_present(writer_data.writerProxy.allLocators,
                                   rtps_udp_locators);
        rtps_udp_locators.length(0);
        append_locator(writer_data.writerProxy.allLocators,
                       param.opendds_locator());
        break;
      case PID_SENTINEL:
      case PID_PAD:
        // ignore
        break;
      default:
        if (param._d() & PIDMASK_INCOMPATIBLE) {
          return -1;
        }
    }
  }
  // Append additional rtps_udp_locators, if any
  append_locators_if_present(writer_data.writerProxy.allLocators,
                             rtps_udp_locators);
  rtps_udp_locators.length(0);
  return 0;
}

int from_param_list(const ParameterList& param_list,
                    Security::DiscoveredWriterData_SecurityWrapper& wrapper)
{
  int result = from_param_list(param_list, wrapper.data) ||
                  from_param_list(param_list, wrapper.security_info, wrapper.data_tags);

  return result;
}

int from_param_list(const ParameterList& param_list,
                    DCPS::DiscoveredReaderData& reader_data)
{
  LocatorState last_state = locator_undefined;  // Track state of locator
  // Collect the rtps_udp locators before appending them to allLocators

  DCPS::LocatorSeq rtps_udp_locators;
  // Start by setting defaults
  reader_data.ddsSubscriptionData.topic_name = "";
  reader_data.ddsSubscriptionData.type_name  = "";
  reader_data.ddsSubscriptionData.durability =
      TheServiceParticipant->initial_DurabilityQosPolicy();
  reader_data.ddsSubscriptionData.deadline =
      TheServiceParticipant->initial_DeadlineQosPolicy();
  reader_data.ddsSubscriptionData.latency_budget =
      TheServiceParticipant->initial_LatencyBudgetQosPolicy();
  reader_data.ddsSubscriptionData.liveliness =
      TheServiceParticipant->initial_LivelinessQosPolicy();
  reader_data.ddsSubscriptionData.reliability =
      TheServiceParticipant->initial_DataReaderQos().reliability;
  reader_data.ddsSubscriptionData.ownership =
      TheServiceParticipant->initial_OwnershipQosPolicy();
  reader_data.ddsSubscriptionData.destination_order =
      TheServiceParticipant->initial_DestinationOrderQosPolicy();
  reader_data.ddsSubscriptionData.user_data =
      TheServiceParticipant->initial_UserDataQosPolicy();
  reader_data.ddsSubscriptionData.time_based_filter =
      TheServiceParticipant->initial_TimeBasedFilterQosPolicy();
  reader_data.ddsSubscriptionData.presentation =
      TheServiceParticipant->initial_PresentationQosPolicy();
  reader_data.ddsSubscriptionData.partition =
      TheServiceParticipant->initial_PartitionQosPolicy();
  reader_data.ddsSubscriptionData.topic_data =
      TheServiceParticipant->initial_TopicDataQosPolicy();
  reader_data.ddsSubscriptionData.group_data =
      TheServiceParticipant->initial_GroupDataQosPolicy();
  reader_data.readerProxy.unicastLocatorList.length(0);
  reader_data.readerProxy.multicastLocatorList.length(0);
  reader_data.readerProxy.expectsInlineQos = false;
  reader_data.contentFilterProperty.contentFilteredTopicName = "";
  reader_data.contentFilterProperty.relatedTopicName = "";
  reader_data.contentFilterProperty.filterClassName = "";
  reader_data.contentFilterProperty.filterExpression = "";
  reader_data.contentFilterProperty.expressionParameters.length(0);

  CORBA::ULong length = param_list.length();
  for (CORBA::ULong i = 0; i < length; ++i) {
    const Parameter& param = param_list[i];
    switch (param._d()) {
      case PID_TOPIC_NAME:
        reader_data.ddsSubscriptionData.topic_name = param.string_data();
        break;
      case PID_TYPE_NAME:
        reader_data.ddsSubscriptionData.type_name = param.string_data();
        break;
      case PID_DURABILITY:
        reader_data.ddsSubscriptionData.durability = param.durability();
        break;
      case PID_DEADLINE:
        reader_data.ddsSubscriptionData.deadline = param.deadline();
        // Interoperability note: calling normalize() shouldn't be required
        normalize(reader_data.ddsSubscriptionData.deadline.period);
        break;
      case PID_LATENCY_BUDGET:
        reader_data.ddsSubscriptionData.latency_budget = param.latency_budget();
        // Interoperability note: calling normalize() shouldn't be required
        normalize(reader_data.ddsSubscriptionData.latency_budget.duration);
        break;
      case PID_LIVELINESS:
        reader_data.ddsSubscriptionData.liveliness = param.liveliness();
        // Interoperability note: calling normalize() shouldn't be required
        normalize(reader_data.ddsSubscriptionData.liveliness.lease_duration);
        break;
      case PID_RELIABILITY:
        reader_data.ddsSubscriptionData.reliability = param.reliability();
        // Interoperability note:
        // Spec creators for RTPS have reliability indexed at 1
        reader_data.ddsSubscriptionData.reliability.kind =
          (DDS::ReliabilityQosPolicyKind)
              ((int)reader_data.ddsSubscriptionData.reliability.kind - 1);
        break;
      case PID_USER_DATA:
        reader_data.ddsSubscriptionData.user_data = param.user_data();
        break;
      case PID_OWNERSHIP:
        reader_data.ddsSubscriptionData.ownership = param.ownership();
        break;
      case PID_DESTINATION_ORDER:
        reader_data.ddsSubscriptionData.destination_order = param.destination_order();
        break;
      case PID_TIME_BASED_FILTER:
        reader_data.ddsSubscriptionData.time_based_filter = param.time_based_filter();
        // Interoperability note: calling normalize() shouldn't be required
        normalize(reader_data.ddsSubscriptionData.time_based_filter.minimum_separation);
        break;
      case PID_PRESENTATION:
        reader_data.ddsSubscriptionData.presentation = param.presentation();
        break;
      case PID_PARTITION:
        reader_data.ddsSubscriptionData.partition = param.partition();
        break;
      case PID_TOPIC_DATA:
        reader_data.ddsSubscriptionData.topic_data = param.topic_data();
        break;
      case PID_GROUP_DATA:
        reader_data.ddsSubscriptionData.group_data = param.group_data();
        break;
      case PID_ENDPOINT_GUID:
        reader_data.readerProxy.remoteReaderGuid = param.guid();
        break;
      case PID_UNICAST_LOCATOR:
        append_locator(rtps_udp_locators, param.locator());
        break;
      case PID_MULTICAST_LOCATOR:
        append_locator(rtps_udp_locators, param.locator());
        break;
      case PID_CONTENT_FILTER_PROPERTY:
        reader_data.contentFilterProperty = param.content_filter_property();
        break;
      case PID_MULTICAST_IPADDRESS:
        set_ipaddress(rtps_udp_locators,
                      last_state,
                      param.ipv4_address());
        break;
      case PID_OPENDDS_LOCATOR:
        // Append the rtps_udp_locators, if any, first, to preserve order
        append_locators_if_present(reader_data.readerProxy.allLocators,
                                   rtps_udp_locators);
        rtps_udp_locators.length(0);
        append_locator(reader_data.readerProxy.allLocators,
                       param.opendds_locator());
        break;
      case PID_OPENDDS_ASSOCIATED_WRITER:
        append_associated_writer(reader_data, param);
        break;
      case PID_SENTINEL:
      case PID_PAD:
        // ignore
        break;
      default:
        if (param._d() & PIDMASK_INCOMPATIBLE) {
          return -1;
        }
    }
  }
  // Append additional rtps_udp_locators, if any
  append_locators_if_present(reader_data.readerProxy.allLocators,
                             rtps_udp_locators);
  rtps_udp_locators.length(0);
  return 0;
}

int from_param_list(const ParameterList& param_list,
                    Security::DiscoveredReaderData_SecurityWrapper& wrapper)
{
  int result = from_param_list(param_list, wrapper.data) ||
                  from_param_list(param_list, wrapper.security_info, wrapper.data_tags);

  return result;
}

} // ParameterListConverter
} // RTPS
} // OpenDDS

OPENDDS_END_VERSIONED_NAMESPACE_DECL
