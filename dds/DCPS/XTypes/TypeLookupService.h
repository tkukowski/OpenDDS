/*
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#ifndef OPENDDS_DCPS_TYPE_LOOKUP_SERVICE_H
#define OPENDDS_DCPS_TYPE_LOOKUP_SERVICE_H

#include "TypeObject.h"

#include <dds/DCPS/RcObject.h>
#include <dds/DCPS/SequenceNumber.h>
#include <dds/DCPS/TypeSupportImpl.h>

#if !defined (ACE_LACKS_PRAGMA_ONCE)
#pragma once
#endif /* ACE_LACKS_PRAGMA_ONCE */

OPENDDS_BEGIN_VERSIONED_NAMESPACE_DECL

namespace OpenDDS {
namespace XTypes {

class OpenDDS_Dcps_Export TypeLookupService : public virtual DCPS::RcObject {
public:
  TypeLookupService();
  ~TypeLookupService();

  // For TypeAssignability
  const TypeObject& get_type_objects(const TypeIdentifier& type_id) const;
  void add_type_objects_to_cache(const TypeIdentifier& ti, const TypeObject& tobj);

  // For Type Lookup Service request / reply
  void get_type_objects(const TypeIdentifierSeq& type_ids,
    TypeIdentifierTypeObjectPairSeq& types) const;
  void add_type_objects_to_cache(const TypeIdentifierTypeObjectPairSeq& types);

  bool get_depend_type_identifiers(const TypeIdentifier& type_id,
    TypeIdentifierWithSizeSeq& dependencies) const;
  void add_depend_type_identifiers(const TypeIdentifier& type_id,
    const TypeIdentifierWithSizeSeq& dependencies);

  // For adding local endpoint types
  void add_type_objects_to_cache(const DCPS::TypeSupportImpl& typesupport);

  bool type_object_in_cache(const TypeIdentifier& ti) const;
  bool type_dependencies_in_cache(const TypeIdentifier& ti) const;

  const DCPS::SequenceNumber& next_rpc_sequence_number()
  {
    return ++rpc_sequence_number_;
  }

  const DCPS::SequenceNumber& rpc_sequence_number() const
  {
    return rpc_sequence_number_;
  }

private:
  // Only minimal Type Objects for now
  typedef std::map<TypeIdentifier, TypeObject> TypeObjectMap;
  TypeObjectMap type_object_map_;

  typedef std::map<TypeIdentifier, TypeIdentifierWithSizeSeq> TypeIdentifierWithSizeSeqMap;
  TypeIdentifierWithSizeSeqMap type_dependencies_map_;

  TypeObject to_empty_;

  DCPS::SequenceNumber rpc_sequence_number_;
};

typedef DCPS::RcHandle<TypeLookupService> TypeLookupService_rch;

} // namespace XTypes
} // namespace OpenDDS

OPENDDS_END_VERSIONED_NAMESPACE_DECL


#endif /* ifndef OPENDDS_DCPS_TYPE_LOOKUP_SERVICE_H */
