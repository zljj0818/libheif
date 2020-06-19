/*
 * HEIF codec.
 * Copyright (c) 2017 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "heif_file.h"
#include "heif_image.h"

#include <fstream>
#include <limits>
#include <sstream>
#include <utility>
#include <string.h>

#include <assert.h>

using namespace heif;


HeifFile::HeifFile()
{
}


HeifFile::~HeifFile()
{
}


std::vector<heif_item_id> HeifFile::get_item_IDs() const
{
  std::vector<heif_item_id> IDs;

  for (const auto& infe : m_infe_boxes) {
    IDs.push_back(infe.second->get_item_ID());
  }

  return IDs;
}


Error HeifFile::read_from_file(const char* input_filename)
{
  auto input_stream_istr = std::unique_ptr<std::istream>(new std::ifstream(input_filename, std::ios_base::binary));
  if (!input_stream_istr->good()) {
    std::stringstream sstr;
    sstr << "Error opening file: " << strerror(errno) << " (" << errno << ")\n";
    return Error(heif_error_Input_does_not_exist, heif_suberror_Unspecified, sstr.str());
  }

  auto input_stream = std::make_shared<StreamReader_istream>(std::move(input_stream_istr));
  return read(input_stream);
}



Error HeifFile::read_from_memory(const void* data, size_t size, bool copy)
{
  auto input_stream = std::make_shared<StreamReader_memory>((const uint8_t*)data, size, copy);

  return read(input_stream);
}


Error HeifFile::read(std::shared_ptr<StreamReader> reader)
{
  m_input_stream = reader;

  uint64_t maxSize = std::numeric_limits<int64_t>::max();
  heif::BitstreamRange range(m_input_stream, maxSize);

  Error error = parse_heif_file(range);
  return error;
}


void HeifFile::new_empty_file()
{
  m_input_stream.reset();
  m_top_level_boxes.clear();

  m_ftyp_box = std::make_shared<Box_ftyp>();
  m_hdlr_box = std::make_shared<Box_hdlr>();
  m_meta_box = std::make_shared<Box_meta>();
  m_ipco_box = std::make_shared<Box_ipco>();
  m_ipma_box = std::make_shared<Box_ipma>();
  m_iloc_box = std::make_shared<Box_iloc>();
  m_iinf_box = std::make_shared<Box_iinf>();
  m_iprp_box = std::make_shared<Box_iprp>();
  m_pitm_box = std::make_shared<Box_pitm>();

  m_meta_box->append_child_box(m_hdlr_box);
  m_meta_box->append_child_box(m_pitm_box);
  m_meta_box->append_child_box(m_iloc_box);
  m_meta_box->append_child_box(m_iinf_box);
  m_meta_box->append_child_box(m_iprp_box);

  m_iprp_box->append_child_box(m_ipco_box);
  m_iprp_box->append_child_box(m_ipma_box);

  m_infe_boxes.clear();

  m_top_level_boxes.push_back(m_ftyp_box);
  m_top_level_boxes.push_back(m_meta_box);
}


void HeifFile::set_brand(heif_compression_format format)
{
  switch (format) {
    case heif_compression_HEVC:
      m_ftyp_box->set_major_brand(fourcc("heic"));
      m_ftyp_box->set_minor_version(0);
      m_ftyp_box->add_compatible_brand(fourcc("mif1"));
      m_ftyp_box->add_compatible_brand(fourcc("heic"));
      break;

    case heif_compression_AV1:
      m_ftyp_box->set_major_brand(fourcc("avif"));
      m_ftyp_box->set_minor_version(0);
      m_ftyp_box->add_compatible_brand(fourcc("avif"));
      m_ftyp_box->add_compatible_brand(fourcc("mif1"));
      break;

  default:
    break;
  }
}


void HeifFile::write(StreamWriter& writer)
{
  for (auto& box : m_top_level_boxes) {
    box->derive_box_version_recursive();
    box->write(writer);
  }

  m_iloc_box->write_mdat_after_iloc(writer);
}


std::string HeifFile::debug_dump_boxes() const
{
  std::stringstream sstr;

  bool first=true;

  for (const auto& box : m_top_level_boxes) {
    // dump box content for debugging

    if (first) {
      first = false;
    }
    else {
      sstr << "\n";
    }

    heif::Indent indent;
    sstr << box->dump(indent);
  }

  return sstr.str();
}


Error HeifFile::parse_heif_file(BitstreamRange& range)
{
  // --- read all top-level boxes

  for (;;) {
    std::shared_ptr<Box> box;
    Error error = Box::read(range, &box);

    // When an EOF error is returned, this is not really a fatal exception,
    // but simply the indication that we reached the end of the file.
    if (error != Error::Ok || range.error() || range.eof()) {
      break;
    }

    m_top_level_boxes.push_back(box);


    // extract relevant boxes (ftyp, meta)

    if (box->get_short_type() == fourcc("meta")) {
      // m_meta_box = std::dynamic_pointer_cast<Box_meta>(box);
      m_meta_box = std::shared_ptr<Box_meta>(reinterpret_cast<Box_meta*>(box.get()));
    }

    if (box->get_short_type() == fourcc("ftyp")) {
      // m_ftyp_box = std::dynamic_pointer_cast<Box_ftyp>(box);
      m_ftyp_box = std::shared_ptr<Box_ftyp>(reinterpret_cast<Box_ftyp*>(box.get()));
    }
  }



  // --- check whether this is a HEIF file and its structural format

  if (!m_ftyp_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_ftyp_box);
  }

  if (!m_ftyp_box->has_compatible_brand(fourcc("heic")) &&
      !m_ftyp_box->has_compatible_brand(fourcc("heix")) &&
      !m_ftyp_box->has_compatible_brand(fourcc("mif1")) &&
      !m_ftyp_box->has_compatible_brand(fourcc("avif"))) {
    std::stringstream sstr;
    sstr << "File does not include any supported brands.\n";

    return Error(heif_error_Unsupported_filetype,
                 heif_suberror_Unspecified,
                 sstr.str());
  }

  if (!m_meta_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_meta_box);
  }

  // m_hdlr_box = std::dynamic_pointer_cast<Box_hdlr>(m_meta_box->get_child_box(fourcc("hdlr")));
  auto hdlr = m_meta_box->get_child_box(fourcc("hdlr"));
  Box_hdlr* hdlr_ptr = hdlr && hdlr->get_short_type() == fourcc("hdlr") ?
          reinterpret_cast<Box_hdlr*>(hdlr.get()) : nullptr;
  m_hdlr_box = std::shared_ptr<Box_hdlr>(hdlr_ptr);
  if (!m_hdlr_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_hdlr_box);
  }

  if (m_hdlr_box->get_handler_type() != fourcc("pict")) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_pict_handler);
  }


  // --- find mandatory boxes needed for image decoding

  // m_pitm_box = std::dynamic_pointer_cast<Box_pitm>(m_meta_box->get_child_box(fourcc("pitm")));
  auto pitm = m_meta_box->get_child_box(fourcc("pitm"));
  Box_pitm* pitm_ptr = pitm && pitm->get_short_type() == fourcc("pitm") ?
          reinterpret_cast<Box_pitm*>(pitm.get()) : nullptr;
  m_pitm_box = std::shared_ptr<Box_pitm>(pitm_ptr);
  if (!m_pitm_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_pitm_box);
  }

  // m_iprp_box = std::dynamic_pointer_cast<Box_iprp>(m_meta_box->get_child_box(fourcc("iprp")));
  auto iprp = m_meta_box->get_child_box(fourcc("iprp"));
  Box_iprp* iprp_ptr = iprp && iprp->get_short_type() == fourcc("iprp") ?
          reinterpret_cast<Box_iprp*>(iprp.get()) : nullptr;
  m_iprp_box = std::shared_ptr<Box_iprp>(iprp_ptr);
  if (!m_iprp_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_iprp_box);
  }

  // m_ipco_box = std::dynamic_pointer_cast<Box_ipco>(m_iprp_box->get_child_box(fourcc("ipco")));
  auto ipco = m_iprp_box->get_child_box(fourcc("ipco"));
  Box_ipco* ipco_ptr = ipco && ipco->get_short_type() == fourcc("ipco") ?
          reinterpret_cast<Box_ipco*>(ipco.get()) : nullptr;
  m_ipco_box = std::shared_ptr<Box_ipco>(ipco_ptr);
  if (!m_ipco_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_ipco_box);
  }

  // m_ipma_box = std::dynamic_pointer_cast<Box_ipma>(m_iprp_box->get_child_box(fourcc("ipma")));
  auto ipma = m_iprp_box->get_child_box(fourcc("ipma"));
  Box_ipma* ipma_ptr = ipma && ipma->get_short_type() == fourcc("ipma") ?
          reinterpret_cast<Box_ipma*>(ipma.get()) : nullptr;
  m_ipma_box = std::shared_ptr<Box_ipma>(ipma_ptr);
  if (!m_ipma_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_ipma_box);
  }

  // m_iloc_box = std::dynamic_pointer_cast<Box_iloc>(m_meta_box->get_child_box(fourcc("iloc")));
  auto iloc = m_meta_box->get_child_box(fourcc("iloc"));
  Box_iloc* iloc_ptr = iloc && iloc->get_short_type() == fourcc("iloc") ?
          reinterpret_cast<Box_iloc*>(iloc.get()) : nullptr;
  m_iloc_box = std::shared_ptr<Box_iloc>(iloc_ptr);
  if (!m_iloc_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_iloc_box);
  }

  // m_idat_box = std::dynamic_pointer_cast<Box_idat>(m_meta_box->get_child_box(fourcc("idat")));
  auto idat = m_meta_box->get_child_box(fourcc("idat"));
  Box_idat* idat_ptr = idat && idat->get_short_type() == fourcc("idat") ?
          reinterpret_cast<Box_idat*>(idat.get()) : nullptr;
  m_idat_box = std::shared_ptr<Box_idat>(idat_ptr);

  // m_iref_box = std::dynamic_pointer_cast<Box_iref>(m_meta_box->get_child_box(fourcc("iref")));
  auto iref = m_meta_box->get_child_box(fourcc("iref"));
  Box_iref* iref_ptr = iref && iref->get_short_type() == fourcc("iref") ?
          reinterpret_cast<Box_iref*>(iref.get()) : nullptr;
  m_iref_box = std::shared_ptr<Box_iref>(iref_ptr);

  // m_iinf_box = std::dynamic_pointer_cast<Box_iinf>(m_meta_box->get_child_box(fourcc("iinf")));
  auto iinf = m_meta_box->get_child_box(fourcc("iinf"));
  Box_iinf* iinf_ptr = iinf && iinf->get_short_type() == fourcc("iinf") ?
          reinterpret_cast<Box_iinf*>(iinf.get()) : nullptr;
  m_iinf_box = std::shared_ptr<Box_iinf>(iinf_ptr);
  if (!m_iinf_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_iinf_box);
  }



  // --- build list of images

  std::vector<std::shared_ptr<Box>> infe_boxes = m_iinf_box->get_child_boxes(fourcc("infe"));

  for (auto& box : infe_boxes) {
    // std::shared_ptr<Box_infe> infe_box = std::dynamic_pointer_cast<Box_infe>(box);
    Box_infe* infe_ptr = box->get_short_type() == fourcc("infe") ?
            reinterpret_cast<Box_infe*>(box.get()) : nullptr;
    std::shared_ptr<Box_infe> infe_box = std::shared_ptr<Box_infe>(infe_ptr);

    if (!infe_box) {
      return Error(heif_error_Invalid_input,
                   heif_suberror_No_infe_box);
    }

    m_infe_boxes.insert( std::make_pair(infe_box->get_item_ID(), infe_box) );
  }

  return Error::Ok;
}


bool HeifFile::image_exists(heif_item_id ID) const
{
  auto image_iter = m_infe_boxes.find(ID);
  return image_iter != m_infe_boxes.end();
}


std::shared_ptr<Box_infe> HeifFile::get_infe(heif_item_id ID) const
{
  // --- get the image from the list of all images

  auto image_iter = m_infe_boxes.find(ID);
  if (image_iter == m_infe_boxes.end()) {
    return nullptr;
  }

  return image_iter->second;
}


std::string HeifFile::get_item_type(heif_item_id ID) const
{
  auto infe_box = get_infe(ID);
  if (!infe_box) {
    return "";
  }

  return infe_box->get_item_type();
}


std::string HeifFile::get_content_type(heif_item_id ID) const
{
  auto infe_box = get_infe(ID);
  if (!infe_box) {
    return "";
  }

  return infe_box->get_content_type();
}


Error HeifFile::get_properties(heif_item_id imageID,
                               std::vector<Box_ipco::Property>& properties) const
{
  if (!m_ipco_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_ipco_box);
  } else if (!m_ipma_box) {
    return Error(heif_error_Invalid_input,
                 heif_suberror_No_ipma_box);
  }

  return m_ipco_box->get_properties_for_item_ID(imageID, m_ipma_box, properties);
}


heif_chroma HeifFile::get_image_chroma_from_configuration(heif_item_id imageID) const
{
  // HEVC

  auto box = m_ipco_box->get_property_for_item_ID(imageID, m_ipma_box, fourcc("hvcC"));
  // std::shared_ptr<Box_hvcC> hvcC_box = std::dynamic_pointer_cast<Box_hvcC>(box);
  Box_hvcC* hvcC_box = box && box->get_short_type() == fourcc("hvcC") ?
          reinterpret_cast<Box_hvcC*>(box.get()) : nullptr;
  // std::shared_ptr<Box_hvcC> hvcC_box = std::shared_ptr<Box_hvcC>(hvcC_ptr);
  if (hvcC_box) {
    return (heif_chroma)(hvcC_box->get_configuration().chroma_format);
  }


  // AV1

  box = m_ipco_box->get_property_for_item_ID(imageID, m_ipma_box, fourcc("av1C"));
  // std::shared_ptr<Box_av1C> av1C_box = std::dynamic_pointer_cast<Box_av1C>(box);
  Box_av1C* av1C_ptr = box && box->get_short_type() == fourcc("av1C") ?
          reinterpret_cast<Box_av1C*>(box.get()) : nullptr;
  std::shared_ptr<Box_av1C> av1C_box = std::shared_ptr<Box_av1C>(av1C_ptr);
  if (av1C_box) {
    Box_av1C::configuration config = av1C_box->get_configuration();
    if (config.chroma_subsampling_x == 1 &&
        config.chroma_subsampling_y == 1) {
      return heif_chroma_420;
    }
    else if (config.chroma_subsampling_x == 1 &&
             config.chroma_subsampling_y == 0) {
      return heif_chroma_422;
    }
    else if (config.chroma_subsampling_x == 0 &&
             config.chroma_subsampling_y == 0) {
      return heif_chroma_444;
    }
    else {
      return heif_chroma_undefined;
    }
  }


  assert(false);
  return heif_chroma_undefined;
}


int HeifFile::get_luma_bits_per_pixel_from_configuration(heif_item_id imageID) const
{
  // HEVC

  auto box = m_ipco_box->get_property_for_item_ID(imageID, m_ipma_box, fourcc("hvcC"));
  // std::shared_ptr<Box_hvcC> hvcC_box = std::dynamic_pointer_cast<Box_hvcC>(box);
  Box_hvcC* hvcC_box = box && box->get_short_type() == fourcc("hvcC") ?
          reinterpret_cast<Box_hvcC*>(box.get()) : nullptr;
  // std::shared_ptr<Box_hvcC> hvcC_box = std::shared_ptr<Box_hvcC>(hvcC_ptr);
  if (hvcC_box) {
    return hvcC_box->get_configuration().bit_depth_luma;
  }

  // AV1

  box = m_ipco_box->get_property_for_item_ID(imageID, m_ipma_box, fourcc("av1C"));
  // std::shared_ptr<Box_av1C> av1C_box = std::dynamic_pointer_cast<Box_av1C>(box);
  Box_av1C* av1C_ptr = box && box->get_short_type() == fourcc("av1C") ?
          reinterpret_cast<Box_av1C*>(box.get()) : nullptr;
  std::shared_ptr<Box_av1C> av1C_box = std::shared_ptr<Box_av1C>(av1C_ptr);
  if (av1C_box) {
    Box_av1C::configuration config = av1C_box->get_configuration();
    if (!config.high_bitdepth) {
      return 8;
    }
    else if (config.twelve_bit) {
      return 12;
    }
    else {
      return 10;
    }
  }

  assert(false);
  return -1;
}


int HeifFile::get_chroma_bits_per_pixel_from_configuration(heif_item_id imageID) const
{
  // HEVC

  auto box = m_ipco_box->get_property_for_item_ID(imageID, m_ipma_box, fourcc("hvcC"));
  // std::shared_ptr<Box_hvcC> hvcC_box = std::dynamic_pointer_cast<Box_hvcC>(box);
  Box_hvcC* hvcC_box = box && box->get_short_type() == fourcc("hvcC") ?
          reinterpret_cast<Box_hvcC*>(box.get()) : nullptr;
  // std::shared_ptr<Box_hvcC> hvcC_box = std::shared_ptr<Box_hvcC>(hvcC_ptr);
  if (hvcC_box) {
    return hvcC_box->get_configuration().bit_depth_chroma;
  }

  // AV1

  box = m_ipco_box->get_property_for_item_ID(imageID, m_ipma_box, fourcc("av1C"));
  // std::shared_ptr<Box_av1C> av1C_box = std::dynamic_pointer_cast<Box_av1C>(box);
  Box_av1C* av1C_ptr = box && box->get_short_type() == fourcc("av1C") ?
          reinterpret_cast<Box_av1C*>(box.get()) : nullptr;
  std::shared_ptr<Box_av1C> av1C_box = std::shared_ptr<Box_av1C>(av1C_ptr);
  if (av1C_box) {
    Box_av1C::configuration config = av1C_box->get_configuration();
    if (!config.high_bitdepth) {
      return 8;
    }
    else if (config.twelve_bit) {
      return 12;
    }
    else {
      return 10;
    }
  }

  assert(false);
  return -1;
}


Error HeifFile::get_compressed_image_data(heif_item_id ID, std::vector<uint8_t>* data) const
{
#if ENABLE_PARALLEL_TILE_DECODING
  std::lock_guard<std::mutex> guard(m_read_mutex);
#endif

  if (!image_exists(ID)) {
    return Error(heif_error_Usage_error,
                 heif_suberror_Nonexisting_item_referenced);
  }

  auto infe_box = get_infe(ID);
  if (!infe_box) {
    return Error(heif_error_Usage_error,
                 heif_suberror_Nonexisting_item_referenced);
  }


  std::string item_type = infe_box->get_item_type();
  std::string content_type = infe_box->get_content_type();

  // --- get coded image data pointers

  auto items = m_iloc_box->get_items();
  const Box_iloc::Item* item = nullptr;
  for (const auto& i : items) {
    if (i.item_ID == ID) {
      item = &i;
      break;
    }
  }
  if (!item) {
    std::stringstream sstr;
    sstr << "Item with ID " << ID << " has no compressed data";

    return Error(heif_error_Invalid_input,
                 heif_suberror_No_item_data,
                 sstr.str());
  }

  Error error = Error(heif_error_Unsupported_feature,
                      heif_suberror_Unsupported_codec);
  if (item_type == "hvc1") {
    // --- --- --- HEVC

    // --- get properties for this image

    std::vector<Box_ipco::Property> properties;
    Error err = m_ipco_box->get_properties_for_item_ID(ID, m_ipma_box, properties);
    if (err) {
      return err;
    }

    // --- get codec configuration

    // std::shared_ptr<Box_hvcC> hvcC_box;
    std::shared_ptr<Box> hvcC_box_wrapper;
    for (auto& prop : properties) {
      if (prop.property->get_short_type() == fourcc("hvcC")) {
        // hvcC_box = std::dynamic_pointer_cast<Box_hvcC>(prop.property);
        // hvcC_box = std::shared_ptr<Box_hvcC>(
        //                 reinterpret_cast<Box_hvcC*>(prop.property.get()));
        hvcC_box_wrapper = prop.property;
        if (hvcC_box_wrapper) {
          break;
        }
      }
    }

    Box_hvcC* hvcC_box = hvcC_box_wrapper ?
        reinterpret_cast<Box_hvcC*>(hvcC_box_wrapper.get()) : nullptr;
    if (!hvcC_box) {
      // Should always have an hvcC box, because we are checking this in
      // heif_context::interpret_heif_file()
      assert(false);
      return Error(heif_error_Invalid_input,
                   heif_suberror_No_hvcC_box);
    } else if (!hvcC_box->get_headers(data)) {
      return Error(heif_error_Invalid_input,
                   heif_suberror_No_item_data);
    }

    error = m_iloc_box->read_data(*item, m_input_stream, m_idat_box, data);
  } else if (item_type == "av01") {
    // --- --- --- AV1

    // --- get properties for this image

    std::vector<Box_ipco::Property> properties;
    Error err = m_ipco_box->get_properties_for_item_ID(ID, m_ipma_box, properties);
    if (err) {
      return err;
    }

    // --- get codec configuration

    std::shared_ptr<Box_av1C> av1C_box;
    for (auto& prop : properties) {
      if (prop.property->get_short_type() == fourcc("av1C")) {
        // av1C_box = std::dynamic_pointer_cast<Box_av1C>(prop.property);
        av1C_box = std::shared_ptr<Box_av1C>(
                        reinterpret_cast<Box_av1C*>(prop.property.get()));
        if (av1C_box) {
          break;
        }
      }
    }

    if (!av1C_box) {
      // Should always have an hvcC box, because we are checking this in
      // heif_context::interpret_heif_file()
      assert(false);
      return Error(heif_error_Invalid_input,
                   heif_suberror_No_hvcC_box);
    } else if (!av1C_box->get_headers(data)) {
      return Error(heif_error_Invalid_input,
                   heif_suberror_No_item_data);
    }

    error = m_iloc_box->read_data(*item, m_input_stream, m_idat_box, data);
  } else if (true ||  // fallback case for all kinds of generic metadata (e.g. 'iptc')
	     item_type == "grid" ||
             item_type == "iovl" ||
             item_type == "Exif" ||
             (item_type == "mime" && content_type=="application/rdf+xml")) {
    error = m_iloc_box->read_data(*item, m_input_stream, m_idat_box, data);
  }

  if (error != Error::Ok) {
    return error;
  }

  return Error::Ok;
}


heif_item_id HeifFile::get_unused_item_id() const
{
  for (heif_item_id id = 1;
       ;
       id++) {

    bool id_exists = false;

    for (const auto& infe : m_infe_boxes) {
      if (infe.second->get_item_ID() == id) {
        id_exists = true;
        break;
      }
    }

    if (!id_exists) {
      return id;
    }
  }

  assert(false); // should never be reached
  return 0;
}


heif_item_id HeifFile::add_new_image(const char* item_type)
{
  auto box = add_new_infe_box(item_type);
  return box->get_item_ID();
}


std::shared_ptr<Box_infe> HeifFile::add_new_infe_box(const char* item_type)
{
  heif_item_id id = get_unused_item_id();

  auto infe = std::make_shared<Box_infe>();
  infe->set_item_ID(id);
  infe->set_hidden_item(false);
  infe->set_item_type(item_type);

  m_infe_boxes[id] = infe;
  m_iinf_box->append_child_box(infe);

  return infe;
}


void HeifFile::add_ispe_property(heif_item_id id, uint32_t width, uint32_t height)
{
  auto ispe = std::make_shared<Box_ispe>();
  ispe->set_size(width,height);

  int index = m_ipco_box->append_child_box(ispe);

  m_ipma_box->add_property_for_item_ID(id, Box_ipma::PropertyAssociation { false, uint16_t(index+1) });
}

void HeifFile::add_hvcC_property(heif_item_id id)
{
  auto hvcC = std::make_shared<Box_hvcC>();
  int index = m_ipco_box->append_child_box(hvcC);

  m_ipma_box->add_property_for_item_ID(id, Box_ipma::PropertyAssociation { true, uint16_t(index+1) });
}


Error HeifFile::append_hvcC_nal_data(heif_item_id id, const std::vector<uint8_t>& nal_data)
{
  // auto hvcC = std::dynamic_pointer_cast<Box_hvcC>(m_ipco_box->get_property_for_item_ID(id,
  //                                                                                     m_ipma_box,
  //                                                                                     fourcc("hvcC")));
  auto box = m_ipco_box->get_property_for_item_ID(id, m_ipma_box, fourcc("hvcC"));
  Box_hvcC* hvcC = box && box->get_short_type() == fourcc("hvcC") ?
          reinterpret_cast<Box_hvcC*>(box.get()) : nullptr;
  // std::shared_ptr<Box_hvcC> hvcC = std::shared_ptr<Box_hvcC>(hvcC_ptr);

  if (hvcC) {
    hvcC->append_nal_data(nal_data);
    return Error::Ok;
  }
  else {
    // Should always have an hvcC box, because we are checking this in
    // heif_context::interpret_heif_file()
    assert(false);
    return Error(heif_error_Usage_error,
                 heif_suberror_No_hvcC_box);
  }
}


Error HeifFile::set_hvcC_configuration(heif_item_id id, const Box_hvcC::configuration& config)
{
  // auto hvcC = std::dynamic_pointer_cast<Box_hvcC>(m_ipco_box->get_property_for_item_ID(id,
  //                                                                                      m_ipma_box,
  //                                                                                      fourcc("hvcC")));
  auto box = m_ipco_box->get_property_for_item_ID(id, m_ipma_box, fourcc("hvcC"));
  Box_hvcC* hvcC = box && box->get_short_type() == fourcc("hvcC") ?
          reinterpret_cast<Box_hvcC*>(box.get()) : nullptr;
  // std::shared_ptr<Box_hvcC> hvcC = std::shared_ptr<Box_hvcC>(hvcC_ptr);

  if (hvcC) {
    hvcC->set_configuration(config);
    return Error::Ok;
  }
  else {
    return Error(heif_error_Usage_error,
                 heif_suberror_No_hvcC_box);
  }
}



Error HeifFile::append_hvcC_nal_data(heif_item_id id, const uint8_t* data, size_t size)
{
  std::vector<Box_ipco::Property> properties;

  // auto hvcC = std::dynamic_pointer_cast<Box_hvcC>(m_ipco_box->get_property_for_item_ID(id,
  //                                                                                      m_ipma_box,
  //                                                                                      fourcc("hvcC")));
  auto box = m_ipco_box->get_property_for_item_ID(id, m_ipma_box, fourcc("hvcC"));
  Box_hvcC* hvcC = box && box->get_short_type() == fourcc("hvcC") ?
          reinterpret_cast<Box_hvcC*>(box.get()) : nullptr;
  // std::shared_ptr<Box_hvcC> hvcC = std::shared_ptr<Box_hvcC>(hvcC_ptr);

  if (hvcC) {
    hvcC->append_nal_data(data,size);
    return Error::Ok;
  }
  else {
    return Error(heif_error_Usage_error,
                 heif_suberror_No_hvcC_box);
  }
}


void HeifFile::add_av1C_property(heif_item_id id)
{
  auto av1C = std::make_shared<Box_av1C>();
  int index = m_ipco_box->append_child_box(av1C);

  m_ipma_box->add_property_for_item_ID(id, Box_ipma::PropertyAssociation { true, uint16_t(index+1) });
}


Error HeifFile::set_av1C_configuration(heif_item_id id, const Box_av1C::configuration& config)
{
  // auto av1C = std::dynamic_pointer_cast<Box_av1C>(m_ipco_box->get_property_for_item_ID(id,
  //                                                                                      m_ipma_box,
  //                                                                                      fourcc("av1C")));
  auto box = m_ipco_box->get_property_for_item_ID(id, m_ipma_box, fourcc("av1C"));
  Box_av1C* av1C_ptr = box && box->get_short_type() == fourcc("av1C") ?
          reinterpret_cast<Box_av1C*>(box.get()) : nullptr;
  std::shared_ptr<Box_av1C> av1C = std::shared_ptr<Box_av1C>(av1C_ptr);

  if (av1C) {
    av1C->set_configuration(config);
    return Error::Ok;
  }
  else {
    return Error(heif_error_Usage_error,
                 heif_suberror_No_av1C_box);
  }
}


void HeifFile::append_iloc_data(heif_item_id id, const std::vector<uint8_t>& nal_packets)
{
  m_iloc_box->append_data(id, nal_packets);
}

void HeifFile::append_iloc_data_with_4byte_size(heif_item_id id, const uint8_t* data, size_t size)
{
  std::vector<uint8_t> nal;
  nal.resize(size + 4);

  nal[0] = (uint8_t)((size>>24) & 0xFF);
  nal[1] = (uint8_t)((size>>16) & 0xFF);
  nal[2] = (uint8_t)((size>> 8) & 0xFF);
  nal[3] = (uint8_t)((size>> 0) & 0xFF);

  memcpy(nal.data()+4, data, size);

  append_iloc_data(id, nal);
}

void HeifFile::set_primary_item_id(heif_item_id id)
{
  m_pitm_box->set_item_ID(id);
}

void HeifFile::add_iref_reference(uint32_t type, heif_item_id from,
                                  std::vector<heif_item_id> to)
{
  if (!m_iref_box) {
    m_iref_box = std::make_shared<Box_iref>();
    m_meta_box->append_child_box(m_iref_box);
  }

  m_iref_box->add_reference(type,from,to);
}

void HeifFile::set_auxC_property(heif_item_id id, std::string type)
{
  auto auxC = std::make_shared<Box_auxC>();
  auxC->set_aux_type(type);

  int index = m_ipco_box->append_child_box(auxC);

  m_ipma_box->add_property_for_item_ID(id, Box_ipma::PropertyAssociation { true, uint16_t(index+1) });
}

void HeifFile::set_color_profile(heif_item_id id, const std::shared_ptr<const color_profile> profile)
{
  auto colr = std::make_shared<Box_colr>();
  colr->set_color_profile(profile);

  int index = m_ipco_box->append_child_box(colr);
  m_ipma_box->add_property_for_item_ID(id, Box_ipma::PropertyAssociation { true, uint16_t(index+1) });
}
