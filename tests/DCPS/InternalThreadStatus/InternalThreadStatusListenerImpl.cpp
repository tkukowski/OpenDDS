/*
 *
 *
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include "InternalThreadStatusListenerImpl.h"

#include <dds/DCPS/ReactorTask.h>

#include <dds/DdsDcpsCoreTypeSupportImpl.h>

#include <ace/streams.h>

#include <string>

// Implementation skeleton constructor
InternalThreadStatusListenerImpl::InternalThreadStatusListenerImpl(OpenDDS::DCPS::String id) :
  id_(id), count_(0), disposes_(0)
{
}

// Implementation skeleton destructor
InternalThreadStatusListenerImpl::~InternalThreadStatusListenerImpl()
{
}

void InternalThreadStatusListenerImpl::on_data_available(DDS::DataReader_ptr reader)
{
  OpenDDS::DCPS::InternalThreadBuiltinTopicDataDataReader_var builtin_dr =
    OpenDDS::DCPS::InternalThreadBuiltinTopicDataDataReader::_narrow(reader);
  if (!builtin_dr) {
    std::cerr << "InternalThreadStatusListenerImpl::"
              << "on_data_available: _narrow failed." << std::endl;
    ACE_OS::exit(1);
  }

  OpenDDS::DCPS::InternalThreadBuiltinTopicData thread_info;
  DDS::SampleInfo si;

  for (DDS::ReturnCode_t status = builtin_dr->read_next_sample(thread_info, si);
       status == DDS::RETCODE_OK;
       status = builtin_dr->read_next_sample(thread_info, si)) {

    std::cout << "== " << id_ << " Thread Info ==" << std::endl;

    if (si.valid_data) {
      OpenDDS::DCPS::RepoId guid;
      std::memcpy(&guid, &thread_info.participant_guid, sizeof(guid));
      std::cout << " guid: " << guid << std::endl;
      ++count_;
    } else if (si.instance_state & DDS::NOT_ALIVE_DISPOSED_INSTANCE_STATE) {
      std::cout << " DISPOSE" << std::endl;
      ++disposes_;
    }

    std::cout
      << "  tid: " << thread_info.thread_id << std::endl
      << " time: " << si.source_timestamp.sec << std::endl;
  }
}

void InternalThreadStatusListenerImpl::on_requested_deadline_missed(
  DDS::DataReader_ptr,
  const DDS::RequestedDeadlineMissedStatus &)
{
  std::cerr << "InternalThreadStatusListenerImpl::"
    << "on_requested_deadline_missed" << std::endl;
}

void InternalThreadStatusListenerImpl::on_requested_incompatible_qos(
  DDS::DataReader_ptr,
  const DDS::RequestedIncompatibleQosStatus &)
{
  std::cerr << "InternalThreadStatusListenerImpl::"
    << "on_requested_incompatible_qos" << std::endl;
}

void InternalThreadStatusListenerImpl::on_liveliness_changed(
  DDS::DataReader_ptr,
  const DDS::LivelinessChangedStatus&)
{
  std::cerr << "InternalThreadStatusListenerImpl::"
    << "on_liveliness_changed" << std::endl;
}

void InternalThreadStatusListenerImpl::on_subscription_matched(
  DDS::DataReader_ptr,
  const DDS::SubscriptionMatchedStatus &)
{
  std::cerr << "InternalThreadStatusListenerImpl::"
    << "on_subscription_matched" << std::endl;
}

void InternalThreadStatusListenerImpl::on_sample_rejected(
  DDS::DataReader_ptr,
  const DDS::SampleRejectedStatus&)
{
  std::cerr << "InternalThreadStatusListenerImpl::"
    << "on_sample_rejected" << std::endl;
}

void InternalThreadStatusListenerImpl::on_sample_lost(
  DDS::DataReader_ptr,
  const DDS::SampleLostStatus&)
{
  std::cerr << "InternalThreadStatusListenerImpl::"
    << "on_sample_lost" << std::endl;
}

int InternalThreadStatusListenerImpl::get_count() const
{
  return count_;
}

size_t InternalThreadStatusListenerImpl::disposes() const
{
  return disposes_;
}
