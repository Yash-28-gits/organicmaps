#include "indexer/classificator.hpp"
#include "indexer/feature_decl.hpp"
#include "indexer/feature_meta.hpp"
#include "indexer/ftypes_matcher.hpp"
#include "indexer/index.hpp"
#include "indexer/osm_editor.hpp"

#include "platform/platform.hpp"

#include "editor/changeset_wrapper.hpp"
#include "editor/osm_auth.hpp"
#include "editor/server_api.hpp"
#include "editor/xml_feature.hpp"

#include "coding/internal/file_data.hpp"

#include "base/logging.hpp"
#include "base/string_utils.hpp"

#include "std/chrono.hpp"
#include "std/future.hpp"
#include "std/tuple.hpp"
#include "std/unordered_map.hpp"
#include "std/unordered_set.hpp"

#include "3party/pugixml/src/pugixml.hpp"

using namespace pugi;
using feature::EGeomType;
using feature::Metadata;
using editor::XMLFeature;

constexpr char const * kEditorXMLFileName = "edits.xml";
constexpr char const * kXmlRootNode = "mapsme";
constexpr char const * kXmlMwmNode = "mwm";
constexpr char const * kDeleteSection = "delete";
constexpr char const * kModifySection = "modify";
constexpr char const * kCreateSection = "create";
/// We store edited streets in OSM-compatible way.
constexpr char const * kAddrStreetTag = "addr:street";

constexpr char const * kUploaded = "Uploaded";
constexpr char const * kDeletedFromOSMServer = "Deleted from OSM by someone";
constexpr char const * kNeedsRetry = "Needs Retry";

namespace osm
{
// TODO(AlexZ): Normalize osm multivalue strings for correct merging
// (e.g. insert/remove spaces after ';' delimeter);

namespace
{
string GetEditorFilePath() { return GetPlatform().WritablePathForFile(kEditorXMLFileName); }
// TODO(mgsergio): Replace hard-coded value with reading from file.
/// type:string -> description:pair<fields:vector<???>, editName:bool, editAddr:bool>

using EType = feature::Metadata::EType;
using TEditableFields = vector<EType>;

struct TypeDescription
{
  TypeDescription(TEditableFields const & fields, bool const name, bool const address) :
      fields(fields),
      name(name),
      address(address)
  {
  }

  TEditableFields const fields;
  bool const name;
  // Address == true implies Street, House Number, Phone, Fax, Opening Hours, Website, EMail, Postcode.
  bool const address;
};

static unordered_map<string, TypeDescription> const gEditableTypes = {
  {"aeroway-aerodrome", {{EType::FMD_ELE, EType::FMD_OPERATOR}, false, true}},
  {"aeroway-airport", {{EType::FMD_ELE, EType::FMD_OPERATOR}, false, true}},
  {"amenity-atm", {{EType::FMD_OPERATOR, EType::FMD_WEBSITE}, true, false}},
  {"amenity-bank", {{EType::FMD_OPERATOR}, true, true}},
  {"amenity-bar", {{EType::FMD_CUISINE, EType::FMD_INTERNET}, true, true}},
  {"amenity-bicycle_rental", {{EType::FMD_OPERATOR}, true, false}},
  {"amenity-bureau_de_change", {{EType::FMD_OPERATOR}, true, true}},
  {"amenity-bus_station", {{EType::FMD_OPERATOR}, true, true}},
  {"amenity-cafe", {{EType::FMD_CUISINE, EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"amenity-car_rental", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"amenity-car_sharing", {{EType::FMD_OPERATOR, EType::FMD_WEBSITE}, true, false}},
  {"amenity-casino", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"amenity-cinema", {{EType::FMD_OPERATOR}, true, true}},
  {"amenity-college", {{EType::FMD_OPERATOR}, true, true}},
  {"amenity-doctors", {{EType::FMD_INTERNET}, true, true}},
  {"amenity-drinking_water", {{}, true, false}},
  {"amenity-embassy", {{}, true, true}},
  {"amenity-fast_food", {{EType::FMD_OPERATOR, EType::FMD_CUISINE}, true, true}},
  {"amenity-ferry_terminal", {{EType::FMD_OPERATOR}, true, true}},
  {"amenity-fire_station", {{}, true, true}},
  {"amenity-fountain", {{}, true, false}},
  {"amenity-fuel", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"amenity-grave_yard", {{}, true, false}},
  {"amenity-hospital", {{}, true, true}},
  {"amenity-hunting_stand", {{EType::FMD_HEIGHT}, true, false}},
  {"amenity-kindergarten", {{EType::FMD_OPERATOR}, true, true}},
  {"amenity-library", {{EType::FMD_INTERNET}, true, true}},
  {"amenity-marketplace", {{EType::FMD_OPERATOR}, true, true}},
  {"amenity-nightclub", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"amenity-parking", {{EType::FMD_OPERATOR}, true, true}},
  {"amenity-pharmacy", {{EType::FMD_OPERATOR}, true, true}},
  {"amenity-place_of_worship", {{}, true, true}},
  {"amenity-police", {{}, true, true}},
  {"amenity-post_box", {{EType::FMD_OPERATOR, EType::FMD_POSTCODE}, true, false}},
  {"amenity-post_office", {{EType::FMD_OPERATOR, EType::FMD_POSTCODE, EType::FMD_INTERNET}, true, true}},
  {"amenity-pub", {{EType::FMD_OPERATOR, EType::FMD_CUISINE, EType::FMD_INTERNET}, true, true}},
  {"amenity-recycling", {{EType::FMD_OPERATOR}, true, false}},
  {"amenity-restaurant", {{EType::FMD_OPERATOR, EType::FMD_CUISINE, EType::FMD_INTERNET}, true, true}},
  {"amenity-school", {{EType::FMD_OPERATOR}, true, true}},
  {"amenity-taxi", {{EType::FMD_OPERATOR}, true, false}},
  {"amenity-telephone", {{EType::FMD_OPERATOR, EType::FMD_PHONE_NUMBER}, false, false}},
  {"amenity-theatre", {{}, true, true}},
  {"amenity-toilets", {{EType::FMD_OPERATOR, EType::FMD_OPEN_HOURS}, true, false}},
  {"amenity-townhall", {{}, true, true}},
  {"amenity-university", {{}, true, true}},
  {"amenity-waste_disposal", {{EType::FMD_OPERATOR, EType::FMD_WEBSITE}, false, false}},
  {"highway-bus_stop", {{EType::FMD_OPERATOR}, true, false}},
  {"historic-archaeological_site", {{EType::FMD_WIKIPEDIA}, true, false}},
  {"historic-castle", {{EType::FMD_WIKIPEDIA}, true, false}},
  {"historic-memorial", {{EType::FMD_WIKIPEDIA}, true, false}},
  {"historic-monument", {{EType::FMD_WIKIPEDIA}, true, false}},
  {"historic-ruins", {{EType::FMD_WIKIPEDIA}, true, false}},
  {"internet-access", {{EType::FMD_INTERNET}, false, false}},
  {"internet-access|wlan", {{EType::FMD_INTERNET}, false, false}},
  {"landuse-cemetery", {{EType::FMD_WIKIPEDIA}, true, false}},
  {"leisure-garden", {{EType::FMD_OPEN_HOURS, EType::FMD_INTERNET}, true, false}},
  {"leisure-sports_centre", {{EType::FMD_INTERNET}, true, true}},
  {"leisure-stadium", {{EType::FMD_WIKIPEDIA, EType::FMD_OPERATOR}, true, true}},
  {"leisure-swimming_pool", {{EType::FMD_OPERATOR}, true, true}},
  {"natural-peak", {{EType::FMD_WIKIPEDIA, EType::FMD_ELE}, true, false}},
  {"natural-spring", {{EType::FMD_WIKIPEDIA}, true, false}},
  {"natural-waterfall", {{EType::FMD_WIKIPEDIA}, true, false}},
  {"office-company", {{}, true, true}},
  {"office-government", {{}, true, true}},
  {"office-lawyer", {{}, true, true}},
  {"office-telecommunication", {{EType::FMD_INTERNET, EType::FMD_OPERATOR}, true, true}},
  {"place-farm", {{EType::FMD_WIKIPEDIA}, true, false}},
  {"place-hamlet", {{EType::FMD_WIKIPEDIA}, true, false}},
  {"place-village", {{EType::FMD_WIKIPEDIA}, true, false}},
  {"railway-halt", {{}, true, false}},
  {"railway-station", {{EType::FMD_OPERATOR}, true, false}},
  {"railway-subway_entrance", {{}, true, false}},
  {"railway-tram_stop", {{EType::FMD_OPERATOR}, true, false}},
  {"shop-alcohol", {{EType::FMD_INTERNET}, true, true}},
  {"shop-bakery", {{EType::FMD_INTERNET}, true, true}},
  {"shop-beauty", {{EType::FMD_INTERNET}, true, true}},
  {"shop-beverages", {{EType::FMD_INTERNET}, true, true}},
  {"shop-bicycle", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-books", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-butcher", {{EType::FMD_INTERNET}, true, true}},
  {"shop-car", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-car_repair", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-chemist", {{EType::FMD_INTERNET}, true, true}},
  {"shop-clothes", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-computer", {{EType::FMD_INTERNET}, true, true}},
  {"shop-confectionery", {{EType::FMD_INTERNET}, true, true }},
  {"shop-convenience", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-department_store", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, false, true}},
  {"shop-doityourself", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-electronics", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-florist", {{EType::FMD_INTERNET}, true, true}},
  {"shop-furniture", {{EType::FMD_INTERNET}, false, true}},
  {"shop-garden_centre", {{EType::FMD_INTERNET}, true, true}},
  {"shop-gift", {{EType::FMD_INTERNET}, true, true}},
  {"shop-greengrocer", {{EType::FMD_INTERNET}, true, true}},
  {"shop-hairdresser", {{EType::FMD_INTERNET}, true, true}},
  {"shop-hardware", {{EType::FMD_INTERNET}, true, true}},
  {"shop-jewelry", {{EType::FMD_INTERNET}, true, true}},
  {"shop-kiosk", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-laundry", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-mall", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-mobile_phone", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-optician", {{EType::FMD_INTERNET}, true, true}},
  {"shop-shoes", {{EType::FMD_INTERNET}, true, true}},
  {"shop-sports", {{EType::FMD_INTERNET}, true, true}},
  {"shop-supermarket", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"shop-toys", {{EType::FMD_INTERNET}, true, true}},
  {"tourism-alpine_hut", {{EType::FMD_ELE, EType::FMD_OPEN_HOURS, EType::FMD_OPERATOR, EType::FMD_WEBSITE, EType::FMD_INTERNET}, true, false}},
  {"tourism-artwork", {{EType::FMD_WEBSITE, EType::FMD_WIKIPEDIA}, true, false}},
  {"tourism-camp_site", {{EType::FMD_OPERATOR, EType::FMD_WEBSITE, EType::FMD_OPEN_HOURS, EType::FMD_INTERNET}, true, false}},
  {"tourism-caravan_site", {{EType::FMD_WEBSITE, EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, false}},
  {"tourism-guest_house", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"tourism-hostel", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"tourism-hotel", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"tourism-information", {{}, true, false}},
  {"tourism-motel", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"tourism-museum", {{EType::FMD_OPERATOR, EType::FMD_INTERNET}, true, true}},
  {"tourism-viewpoint", {{}, true, false}},
  {"waterway-waterfall", {{EType::FMD_HEIGHT}, true, false}}};

TypeDescription const * GetTypeDescription(uint32_t const type)
{
  auto const readableType = classif().GetReadableObjectName(type);
  auto const it = gEditableTypes.find(readableType);
  if (it != end(gEditableTypes))
    return &it->second;
  return nullptr;
}

uint32_t MigrateFeatureIndex(XMLFeature const & /*xml*/)
{
  // @TODO(mgsergio): Update feature's index when user has downloaded fresh MWM file and old indices point to other features.
  // Possible implementation: use function to load features in rect (center feature's point) and somehow compare/choose from them.
  // Probably we need to store more data about features in xml, e.g. types, may be other data, to match them correctly.
  return 0;
}

} // namespace

Editor & Editor::Instance()
{
  static Editor instance;
  return instance;
}

void Editor::LoadMapEdits()
{
  if (!m_mwmIdByMapNameFn)
  {
    LOG(LERROR, ("Can't load any map edits, MwmIdByNameAndVersionFn has not been set."));
    return;
  }

  xml_document doc;
  {
    string const fullFilePath = GetEditorFilePath();
    xml_parse_result const res = doc.load_file(fullFilePath.c_str());
    // Note: status_file_not_found is ok if user has never made any edits.
    if (res != status_ok && res != status_file_not_found)
    {
      LOG(LERROR, ("Can't load map edits from disk:", fullFilePath));
      return;
    }
  }

  array<pair<FeatureStatus, char const *>, 3> const sections =
  {{
      {FeatureStatus::Deleted, kDeleteSection},
      {FeatureStatus::Modified, kModifySection},
      {FeatureStatus::Created, kCreateSection}
  }};
  int deleted = 0, modified = 0, created = 0;

  for (xml_node mwm : doc.child(kXmlRootNode).children(kXmlMwmNode))
  {
    string const mapName = mwm.attribute("name").as_string("");
    int64_t const mapVersion = mwm.attribute("version").as_llong(0);
    MwmSet::MwmId const id = m_mwmIdByMapNameFn(mapName);
    if (!id.IsAlive())
    {
      // TODO(AlexZ): MWM file was deleted, but changes have left. What whould we do in this case?
      LOG(LWARNING, (mapName, "version", mapVersion, "references not existing MWM file."));
      continue;
    }

    for (auto const & section : sections)
    {
      for (auto const nodeOrWay : mwm.child(section.second).select_nodes("node|way"))
      {
        try
        {
          XMLFeature const xml(nodeOrWay.node());
          uint32_t const featureIndex = mapVersion < id.GetInfo()->GetVersion() ? xml.GetMWMFeatureIndex() : MigrateFeatureIndex(xml);
          FeatureID const fid(id, featureIndex);

          FeatureTypeInfo fti;

          /// TODO(mgsergio): uncomment when feature creating will
          /// be required
          // if (xml.GetType() != XMLFeature::Type::Way)
          // {
          // TODO(mgsergio): Check if feature can be read.
          fti.m_feature = *m_featureLoaderFn(fid);
          fti.m_feature.ApplyPatch(xml);
          // }
          // else
          // {
          //   fti.m_feature = FeatureType::FromXML(xml);
          // }

          fti.m_feature.SetID(fid);
          fti.m_street = xml.GetTagValue(kAddrStreetTag);

          fti.m_modificationTimestamp = xml.GetModificationTime();
          ASSERT_NOT_EQUAL(my::INVALID_TIME_STAMP, fti.m_modificationTimestamp, ());
          fti.m_uploadAttemptTimestamp = xml.GetUploadTime();
          fti.m_uploadStatus = xml.GetUploadStatus();
          fti.m_uploadError = xml.GetUploadError();
          fti.m_status = section.first;

          /// Call to m_featureLoaderFn indirectly tries to load feature by
          /// it's ID from the editor's m_features.
          /// That's why insertion into m_features should go AFTER call to m_featureLoaderFn.
          m_features[id][fid.m_index] = fti;
        }
        catch (editor::XMLFeatureError const & ex)
        {
          ostringstream s;
          nodeOrWay.node().print(s, "  ");
          LOG(LERROR, (ex.what(), "Can't create XMLFeature in section", section.second, s.str()));
        }
      } // for nodes
    } // for sections
  } // for mwms

  LOG(LINFO, ("Loaded", modified, "modified,", created, "created and", deleted, "deleted features."));
}

void Editor::Save(string const & fullFilePath) const
{
  // Should we delete edits file if user has canceled all changes?
  if (m_features.empty())
    return;

  xml_document doc;
  xml_node root = doc.append_child(kXmlRootNode);
  // Use format_version for possible future format changes.
  root.append_attribute("format_version") = 1;
  for (auto const & mwm : m_features)
  {
    xml_node mwmNode = root.append_child(kXmlMwmNode);
    mwmNode.append_attribute("name") = mwm.first.GetInfo()->GetCountryName().c_str();
    mwmNode.append_attribute("version") = static_cast<long long>(mwm.first.GetInfo()->GetVersion());
    xml_node deleted = mwmNode.append_child(kDeleteSection);
    xml_node modified = mwmNode.append_child(kModifySection);
    xml_node created = mwmNode.append_child(kCreateSection);
    for (auto const & index : mwm.second)
    {
      FeatureTypeInfo const & fti = index.second;
      XMLFeature xf = fti.m_feature.ToXML();
      xf.SetMWMFeatureIndex(index.first);
      if (!fti.m_street.empty())
        xf.SetTagValue(kAddrStreetTag, fti.m_street);
      ASSERT_NOT_EQUAL(0, fti.m_modificationTimestamp, ());
      xf.SetModificationTime(fti.m_modificationTimestamp);
      if (fti.m_uploadAttemptTimestamp != my::INVALID_TIME_STAMP)
      {
        xf.SetUploadTime(fti.m_uploadAttemptTimestamp);
        ASSERT(!fti.m_uploadStatus.empty(), ("Upload status updates with upload timestamp."));
        xf.SetUploadStatus(fti.m_uploadStatus);
        if (!fti.m_uploadError.empty())
          xf.SetUploadError(fti.m_uploadError);
      }
      switch (fti.m_status)
      {
      case FeatureStatus::Deleted: VERIFY(xf.AttachToParentNode(deleted), ()); break;
      case FeatureStatus::Modified: VERIFY(xf.AttachToParentNode(modified), ()); break;
      case FeatureStatus::Created: VERIFY(xf.AttachToParentNode(created), ()); break;
      case FeatureStatus::Untouched: CHECK(false, ("Not edited features shouldn't be here."));
      }
    }
  }

  if (doc)
  {
    auto const & tmpFileName = fullFilePath + ".tmp";
    if (!doc.save_file(tmpFileName.data(), "  "))
      LOG(LERROR, ("Can't save map edits into", tmpFileName));
    else if (!my::RenameFileX(tmpFileName, fullFilePath))
      LOG(LERROR, ("Can't rename file", tmpFileName, "to", fullFilePath));
  }
}

Editor::FeatureStatus Editor::GetFeatureStatus(MwmSet::MwmId const & mwmId, uint32_t index) const
{
  // Most popular case optimization.
  if (m_features.empty())
    return FeatureStatus::Untouched;

  auto const mwmMatched = m_features.find(mwmId);
  if (mwmMatched == m_features.end())
    return FeatureStatus::Untouched;

  auto const matchedIndex = mwmMatched->second.find(index);
  if (matchedIndex == mwmMatched->second.end())
    return FeatureStatus::Untouched;

  return matchedIndex->second.m_status;
}

void Editor::DeleteFeature(FeatureType const & feature)
{
  FeatureID const fid = feature.GetID();
  FeatureTypeInfo & ftInfo = m_features[fid.m_mwmId][fid.m_index];
  ftInfo.m_status = FeatureStatus::Deleted;
  ftInfo.m_feature = feature;
  // TODO: What if local client time is absolutely wrong?
  ftInfo.m_modificationTimestamp = time(nullptr);

  // TODO(AlexZ): Synchronize Save call/make it on a separate thread.
  Save(GetEditorFilePath());

  if (m_invalidateFn)
    m_invalidateFn();
}

//namespace
//{
//FeatureID GenerateNewFeatureId(FeatureID const & oldFeatureId)
//{
//  // TODO(AlexZ): Stable & unique features ID generation.
//  static uint32_t newIndex = 0x0effffff;
//  return FeatureID(oldFeatureId.m_mwmId, newIndex++);
//}
//}  // namespace

void Editor::EditFeature(FeatureType const & editedFeature, string const & editedStreet)
{
  // TODO(AlexZ): Check if feature has not changed and reset status.
  FeatureID const fid = editedFeature.GetID();
  FeatureTypeInfo & fti = m_features[fid.m_mwmId][fid.m_index];
  fti.m_status = FeatureStatus::Modified;
  fti.m_feature = editedFeature;
  // TODO: What if local client time is absolutely wrong?
  fti.m_modificationTimestamp = time(nullptr);

  if (!editedStreet.empty())
    fti.m_street = editedStreet;

  // TODO(AlexZ): Synchronize Save call/make it on a separate thread.
  Save(GetEditorFilePath());

  if (m_invalidateFn)
    m_invalidateFn();
}

void Editor::ForEachFeatureInMwmRectAndScale(MwmSet::MwmId const & id,
                                             TFeatureIDFunctor const & f,
                                             m2::RectD const & rect,
                                             uint32_t /*scale*/)
{
  auto const mwmFound = m_features.find(id);
  if (mwmFound == m_features.end())
    return;

  // TODO(AlexZ): Check that features are visible at this scale.
  // Process only new (created) features.
  for (auto const & index : mwmFound->second)
  {
    FeatureTypeInfo const & ftInfo = index.second;
    if (ftInfo.m_status == FeatureStatus::Created &&
        rect.IsPointInside(ftInfo.m_feature.GetCenter()))
      f(FeatureID(id, index.first));
  }
}

void Editor::ForEachFeatureInMwmRectAndScale(MwmSet::MwmId const & id,
                                             TFeatureTypeFunctor const & f,
                                             m2::RectD const & rect,
                                             uint32_t /*scale*/)
{
  auto mwmFound = m_features.find(id);
  if (mwmFound == m_features.end())
    return;

  // TODO(AlexZ): Check that features are visible at this scale.
  // Process only new (created) features.
  for (auto & index : mwmFound->second)
  {
    FeatureTypeInfo & ftInfo = index.second;
    if (ftInfo.m_status == FeatureStatus::Created &&
        rect.IsPointInside(ftInfo.m_feature.GetCenter()))
      f(ftInfo.m_feature);
  }
}

bool Editor::GetEditedFeature(MwmSet::MwmId const & mwmId, uint32_t index, FeatureType & outFeature) const
{
  auto const mwmMatched = m_features.find(mwmId);
  if (mwmMatched == m_features.end())
    return false;

  auto const matchedIndex = mwmMatched->second.find(index);
  if (matchedIndex == mwmMatched->second.end())
    return false;

  // TODO(AlexZ): Should we process deleted/created features as well?
  outFeature = matchedIndex->second.m_feature;
  return true;
}

vector<Metadata::EType> Editor::EditableMetadataForType(FeatureType const & feature) const
{
  // TODO(mgsergio): Load editable fields into memory from XML and query them here.
  feature::TypesHolder const types(feature);
  set<Metadata::EType> fields;
  for (auto type : types)
  {
    auto const * desc = GetTypeDescription(type);
    if (desc)
    {
      for (auto field : desc->fields)
        fields.insert(field);
      // If address is editable, many metadata fields are editable too.
      if (desc->address)
      {
        fields.insert(EType::FMD_EMAIL);
        fields.insert(EType::FMD_OPEN_HOURS);
        fields.insert(EType::FMD_PHONE_NUMBER);
        fields.insert(EType::FMD_POSTCODE);
        fields.insert(EType::FMD_WEBSITE);
      }
    }
  }
  return {begin(fields), end(fields)};
}

bool Editor::IsNameEditable(FeatureType const & feature) const
{
  feature::TypesHolder const types(feature);
  for (auto type : types)
  {
    auto const * typeDesc = GetTypeDescription(type);
    if (typeDesc && typeDesc->name)
      return true;
  }

  return false;
}

bool Editor::IsAddressEditable(FeatureType const & feature) const
{
  feature::TypesHolder const types(feature);
  for (auto type : types)
  {
    // Building addresses are always editable.
    if (ftypes::IsBuildingChecker::Instance().HasTypeValue(type))
      return true;
    auto const * typeDesc = GetTypeDescription(type);
    if (typeDesc && typeDesc->address)
      return true;
  }

  return false;
}

void Editor::UploadChanges(string const & key, string const & secret, TChangesetTags const & tags)
{
  // TODO(AlexZ): features access should be synchronized.
  auto const lambda = [this](string key, string secret, TChangesetTags tags)
  {
    int uploadedFeaturesCount = 0;
    // TODO(AlexZ): insert usefull changeset comments.
    ChangesetWrapper changeset({key, secret}, tags);
    for (auto & id : m_features)
    {
      for (auto & index : id.second)
      {
        FeatureTypeInfo & fti = index.second;
        // Do not process already uploaded features or those failed permanently.
        if (!(fti.m_uploadStatus.empty() || fti.m_uploadStatus == kNeedsRetry))
          continue;

        // TODO(AlexZ): Create/delete nodes support.
        if (fti.m_status != FeatureStatus::Modified)
          continue;

        XMLFeature feature = fti.m_feature.ToXML();
        // TODO(AlexZ): Add areas(ways) upload support.
        if (feature.GetType() != XMLFeature::Type::Node)
          continue;

        try
        {
          XMLFeature osmFeature = changeset.GetMatchingFeatureFromOSM(feature, fti.m_feature);
          XMLFeature const osmFeatureCopy = osmFeature;
          osmFeature.ApplyPatch(feature);
          // Check to avoid duplicates.
          if (osmFeature == osmFeatureCopy)
          {
            LOG(LWARNING, ("Local changes are equal to OSM, feature was not uploaded, local changes were deleted.", feature));
            // TODO(AlexZ): Delete local change.
            continue;
          }
          LOG(LDEBUG, ("Uploading patched feature", osmFeature));
          changeset.ModifyNode(osmFeature);
          fti.m_uploadStatus = kUploaded;
          fti.m_uploadAttemptTimestamp = time(nullptr);
          ++uploadedFeaturesCount;
        }
        catch (ChangesetWrapper::OsmObjectWasDeletedException const & ex)
        {
          fti.m_uploadStatus = kDeletedFromOSMServer;
          fti.m_uploadAttemptTimestamp = time(nullptr);
          fti.m_uploadError = "Node was deleted from the server.";
          LOG(LWARNING, (fti.m_uploadError, ex.what()));
        }
        catch (RootException const & ex)
        {
          LOG(LWARNING, (ex.what()));
          fti.m_uploadStatus = kNeedsRetry;
          fti.m_uploadAttemptTimestamp = time(nullptr);
          fti.m_uploadError = ex.what();
        }
        // TODO(AlexZ): Synchronize save after edits.
        // Call Save every time we modify each feature's information.
        Save(GetEditorFilePath());
      }
    }
    // TODO(AlexZ): Should we call any callback at the end?
  };

  // Do not run more than one upload thread at a time.
  static auto future = async(launch::async, lambda, key, secret, tags);
  auto const status = future.wait_for(milliseconds(0));
  if (status == future_status::ready)
    future = async(launch::async, lambda, key, secret, tags);
}

}  // namespace osm
