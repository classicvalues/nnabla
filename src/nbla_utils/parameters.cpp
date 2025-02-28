// Copyright 2019,2020,2021 Sony Corporation.
// Copyright 2021 Sony Group Corporation.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// Include nnabla header files

#include <assert.h>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdio.h>
#include <string>
#include <vector>

#include <nbla/computation_graph/variable.hpp>
#include <nbla/initializer.hpp>
#include <nbla/logger.hpp>
#include <nbla/parametric_functions.hpp>
#include <nbla/std.hpp>

// Protobuf
#include <google/protobuf/descriptor.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>

#include "nbla_utils/parameters.hpp"
#include "nnabla.pb.h"
#include "parameters_impl.hpp"

// HDF5
#ifdef NBLA_UTILS_WITH_HDF5
#include <hdf5.h>
#include <hdf5_hl.h>
#endif

using namespace std;

namespace nbla {
namespace utils {
namespace { // Internal functions and variables

const nbla::Context cpu_ctx{{"cpu:float"}, "CpuCachedArray", "0"};
const int MAX_NAME = 1024;

string get_extension(const string &filename) {
  int ep = filename.find_last_of(".");
  if (ep < 0) {
    NBLA_ERROR(error_code::value, "File: %s has no extension name.",
               filename.c_str());
  }
  string ext = filename.substr(ep, filename.size() - ep);
  return ext;
}

#ifdef NBLA_UTILS_WITH_HDF5
bool parse_hdf5_dataset(string name, hid_t did, ParameterVector &pv) {
  hid_t sp = H5Dget_space(did);
  int rank = H5Sget_simple_extent_ndims(sp);
  hsize_t dims[rank];
  herr_t err = H5Sget_simple_extent_dims(sp, dims, nullptr);

  hsize_t size = H5Dget_storage_size(did);
  string variable_name = name.substr(1, name.length());

  NBLA_LOG_INFO("Dataset Name:[{}] type: {} size: {}", variable_name, t_class,
                size);

  auto buffer = make_unique<float[]>(size / sizeof(float));
  assert(buffer);
  // TODO: Other data types than float.
  err = H5Dread(did, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                buffer.get());
  if (err >= 0) {
    Shape_t shape(dims, dims + rank);
    // fix crash bug by replacing bool with int,
    // since actual 4 bytes is read.
    int need_grad = false; // default need_grad
    if (H5Aexists(did, "need_grad")) {
      hid_t att = H5Aopen(did, "need_grad", H5P_DEFAULT);
      H5Aread(att, H5T_NATIVE_HBOOL, &need_grad);
      H5Aclose(att);
    }

    CgVariablePtr cg_v = make_shared<CgVariable>(shape, need_grad);
    float *data =
        cg_v->variable()->template cast_data_and_get_pointer<float>(cpu_ctx);
    for (unsigned int i = 0; i < size / sizeof(float); i++) {
      data[i] = buffer[i];
    }
    pv.push_back({variable_name, cg_v});
    return true;
  }
  NBLA_ERROR(error_code::not_implemented, "HDF5 is not enabled when build.");
  return false;
}

bool parse_hdf5_group(hid_t gid, ParameterVector &pv) {
  ssize_t len;
  hsize_t num = 0;
  herr_t err = H5Gget_num_objs(gid, &num);
  if (err >= 0) {
    char group_name[MAX_NAME];
    len = H5Iget_name(gid, group_name, MAX_NAME);
    if (len >= 0) {
      for (unsigned int i = 0; i < num; i++) {
        char name[MAX_NAME];
        len = H5Gget_objname_by_idx(gid, (hsize_t)i, name, (size_t)MAX_NAME);
        if (len < 0) {
          return false;
        }

        int type = H5Gget_objtype_by_idx(gid, i);
        switch (type) {
        case H5G_GROUP: {
          hid_t grpid = H5Gopen(gid, name, H5P_DEFAULT);
          parse_hdf5_group(grpid, pv);
          H5Gclose(grpid);
          break;
        }
        case H5G_DATASET: {
          hid_t did = H5Dopen(gid, name, H5P_DEFAULT);
          string dataset_name(group_name);
          if (dataset_name != "/")
            dataset_name += "/";
          dataset_name += string(name);
          parse_hdf5_dataset(dataset_name, did, pv);
          H5Dclose(did);
          break;
        }
        case H5G_TYPE:
          NBLA_LOG_INFO("H5G_TYPE");
          break;
        case H5G_LINK:
          NBLA_LOG_INFO("H5G_LINK");
          break;
        default:
          // TODO: Unsupported member.
          NBLA_LOG_INFO("default");
          break;
        }
      }
      return true;
    }
  }
  return false;
}

void create_h5_group(hid_t file_id, const string &filename) {
  hid_t group_id;
  int ep = filename.find_last_of("/");
  if (ep == -1) {
    return;
  } else {
    H5G_info_t group_info;
    string base_name = filename.substr(0, ep);
    herr_t err = H5Gget_info_by_name(file_id, base_name.c_str(), &group_info,
                                     H5P_DEFAULT);
    if (err >= 0) {
      return;
    } else {
      create_h5_group(file_id, base_name);
      group_id = H5Gcreate2(file_id, base_name.c_str(), H5P_DEFAULT,
                            H5P_DEFAULT, H5P_DEFAULT);
      if (group_id < 0) {
        NBLA_ERROR(error_code::value, "Cannot create dataset %s in .h5 file.",
                   base_name.c_str());
      } else {
        H5Gclose(group_id);
      }
    }
  }
}

void create_h5_dataset(const ParameterVector &pv, hid_t file_id) {
  hid_t dataset_id, dataspace_id;
  hid_t attr_space_id, attr_id;
  herr_t status;
  int index = 0;

  for (auto it = pv.begin(); it != pv.end(); it++, index++) {
    string name = it->first;
    CgVariablePtr cg_v = it->second;
    VariablePtr variable = cg_v->variable();
    Shape_t shape = variable->shape();
    dataspace_id =
        H5Screate_simple(shape.size(), (hsize_t *)shape.data(), NULL);
    create_h5_group(file_id, name);
    dataset_id =
        H5Dcreate2(file_id, name.c_str(), H5T_NATIVE_FLOAT, dataspace_id,
                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    float *data = variable->template cast_data_and_get_pointer<float>(cpu_ctx);
    status = H5Dwrite(dataset_id, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL,
                      H5P_DEFAULT, data);
    if (status >= 0) {
      attr_space_id = H5Screate(H5S_SCALAR);
      int need_grad = cg_v->need_grad();
      attr_id = H5Acreate2(dataset_id, "need_grad", H5T_NATIVE_INT,
                           attr_space_id, H5P_DEFAULT, H5P_DEFAULT);
      H5Awrite(attr_id, H5T_NATIVE_INT, &need_grad);
      H5Aclose(attr_id);
      H5Sclose(attr_space_id);
      attr_space_id = H5Screate(H5S_SCALAR);
      attr_id = H5Acreate2(dataset_id, "index", H5T_NATIVE_INT, attr_space_id,
                           H5P_DEFAULT, H5P_DEFAULT);
      H5Awrite(attr_id, H5T_NATIVE_INT, &index);
      H5Aclose(attr_id);
      H5Sclose(attr_space_id);
    } else {
      NBLA_ERROR(error_code::not_implemented,
                 "Cannot write h5 file's dataset.");
    }
    H5Dclose(dataset_id);
    H5Sclose(dataspace_id);
  }
}

#endif

void load_parameters_from_proto(NNablaProtoBuf &param, ParameterVector &pv) {
  for (auto it = param.parameter().begin(); it != param.parameter().end();
       it++) {
    const string &name = it->variable_name();
    Shape_t shape(it->shape().dim().begin(), it->shape().dim().end());
    bool need_grad = it->need_grad();
    CgVariablePtr cg_v = make_shared<CgVariable>(shape, need_grad);
    float *data =
        cg_v->variable()->template cast_data_and_get_pointer<float>(cpu_ctx);
    auto &p_data = it->data();
    NBLA_CHECK(p_data.size() == cg_v->variable()->size(), error_code::value,
               "Inconsistent size in proto parameter %s (%d != %d)",
               name.c_str(), (int)p_data.size(), (int)cg_v->variable()->size());
    for (int i = 0; i < p_data.size(); i++) {
      data[i] = p_data[i];
    }
    pv.push_back({name, cg_v});
  }
}

} // private functions and variables

// ----------------------------------------------------------------------
// load parameters from .h5 file buffer
// ----------------------------------------------------------------------
bool load_parameters_h5(ParameterVector &pv, char *buffer, int size) {
#ifdef NBLA_UTILS_WITH_HDF5
  bool ret = false;
  hid_t id = H5LTopen_file_image(buffer, size, H5LT_FILE_IMAGE_DONT_RELEASE);
  if (id >= 0) {
    hid_t root_id = H5Gopen(id, "/", H5P_DEFAULT);
    if (root_id >= 0) {
      ret = parse_hdf5_group(root_id, pv);
      H5Gclose(root_id);
    } else {
      NBLA_ERROR(error_code::value, "Cannot found group in buffer.");
    }
    H5Fclose(id);
  }
  return ret;
#else
  NBLA_ERROR(
      error_code::not_implemented,
      "Cannot load parameters from .h5. HDF5 might not enabled when build.");
  return false;
#endif
}

// ----------------------------------------------------------------------
// load parameters from .h5 file
// ----------------------------------------------------------------------
bool load_parameters_h5(ParameterVector &pv, string filename) {
  std::ifstream file(filename.c_str(), std::ios::binary | std::ios::ate);
  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  vector<char> buffer(size);
  if (file.read(buffer.data(), size)) {
    return load_parameters_h5(pv, buffer.data(), size);
  }
  return false;
}

// ----------------------------------------------------------------------
// load parameters from protobuf file's buffer
// ----------------------------------------------------------------------
bool load_parameters_pb(ParameterVector &pv, char *buffer, int size) {
  constexpr int size_1024_mb = 1024 << 20;
#if GOOGLE_PROTOBUF_VERSION < 3006001
  constexpr int size_128_mb = 128 << 20;
#endif
  NNablaProtoBuf param;
  google::protobuf::io::ArrayInputStream input(buffer, size);
  google::protobuf::io::CodedInputStream coded_input(&input);
#if GOOGLE_PROTOBUF_VERSION < 3006001
  coded_input.SetTotalBytesLimit(size_1024_mb, size_128_mb);
#else
  coded_input.SetTotalBytesLimit(size_1024_mb);
#endif
  bool ret = param.ParseFromCodedStream(&coded_input);
  if (!ret) {
    NBLA_ERROR(error_code::value, "Cannot load parameters from buffer!");
    return false;
  }

  load_parameters_from_proto(param, pv);
  return true;
}

// ----------------------------------------------------------------------
// load parameters from protobuf file
// ----------------------------------------------------------------------
bool load_parameters_pb(ParameterVector &pv, string filename) {
  NNablaProtoBuf param;
  std::ifstream ifs(filename.c_str(), std::ios::binary);
  if (!ifs.is_open()) {
    NBLA_LOG_WARN("Error in opening file {}", filename);
    return false;
  }
  param.ParseFromIstream(&ifs);

  load_parameters_from_proto(param, pv);
  return true;
}

// ----------------------------------------------------------------------
// load parameters from file which is specified by filename
// choose the format by its file extension name.
// ----------------------------------------------------------------------
bool load_parameters(ParameterVector &pv, string filename) {
  bool ret = false;
  string ext = get_extension(filename);
  if (ext == ".h5") {
    ret = load_parameters_h5(pv, filename);
  } else if (ext == ".protobuf") {
    ret = load_parameters_pb(pv, filename);
  } else {
    NBLA_ERROR(error_code::value, "Not supported file extension: %s",
               filename.c_str());
  }
  return ret;
}

// ----------------------------------------------------------------------
// Load parameters from file
// ----------------------------------------------------------------------
bool load_parameters(ParameterDirectory &pd, string filename) {
  ParameterVector pv;
  bool ret = load_parameters(pv, filename);

  if (!ret) {
    NBLA_LOG_INFO("Cannot load parameter file: %s\n", filename.c_str());
    return false;
  }

  for (auto it = pv.begin(); it != pv.end(); ++it) {
    pd.get_parameter_or_create(it->first, it->second);
  }
  NBLA_LOG_INFO("Load parameters from {}", filename);

  return true;
}

// ----------------------------------------------------------------------
// Load parameters from h5 file buffer
// ----------------------------------------------------------------------
bool load_parameters_h5(ParameterDirectory &pd, char *buf, int size) {
  ParameterVector pv;
  bool ret = load_parameters_h5(pv, buf, size);

  if (!ret) {
    NBLA_LOG_INFO("Cannot load parameter file buffer!\n");
    return false;
  }

  for (auto it = pv.begin(); it != pv.end(); ++it) {
    pd.get_parameter_or_create(it->first, it->second);
  }

  return true;
}

// ----------------------------------------------------------------------
// Load parameters from pb file buffer
// ----------------------------------------------------------------------
bool load_parameters_pb(ParameterDirectory &pd, char *buf, int size) {
  ParameterVector pv;
  bool ret = load_parameters_pb(pv, buf, size);

  if (!ret) {
    NBLA_LOG_INFO("Cannot load parameter file buffer!\n");
    return false;
  }

  for (auto it = pv.begin(); it != pv.end(); ++it) {
    pd.get_parameter_or_create(it->first, it->second);
  }

  return true;
}

// ----------------------------------------------------------------------
// save parameters as pb to a stream output
// ----------------------------------------------------------------------
void save_parameters_pb_to_stream(const ParameterVector &pv, ostream *ofs) {
  NNablaProtoBuf params;
  for (auto it = pv.begin(); it != pv.end(); it++) {

    string name = it->first;
    VariablePtr variable = it->second->variable();
    Parameter *parameter = params.add_parameter();
    parameter->set_variable_name(name);
    CgVariablePtr cg_var = it->second;
    parameter->set_need_grad(cg_var->need_grad());

    float *data = variable->template cast_data_and_get_pointer<float>(cpu_ctx);
    for (int i = 0; i < variable->size(); i++)
      parameter->add_data(data[i]);

    Shape *shape = parameter->mutable_shape();
    for (Shape_t::size_type i = 0; i < variable->shape().size(); i++)
      shape->add_dim(variable->shape()[i]);
  }
  params.SerializeToOstream(ofs);
}

// ----------------------------------------------------------------------
// save parameters to a .h5 file specified by filename
// ----------------------------------------------------------------------
bool save_parameters_h5(const ParameterVector &pv, string filename) {
#ifdef NBLA_UTILS_WITH_HDF5
  hid_t file_id;

  H5Eset_auto1(NULL, NULL);
  file_id =
      H5Fcreate(filename.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  create_h5_dataset(pv, file_id);
  H5Fclose(file_id);
  return true;
#else
  // Not implemented
  NBLA_ERROR(error_code::not_implemented,
             "Cannot write dataset to .h5. HDF5 might not enabled when build.");
  return false;
#endif
}

// ----------------------------------------------------------------------
// save parameters to a .protobuf file specified by filename
// ----------------------------------------------------------------------
bool save_parameters_pb(const ParameterVector &pv, string filename) {
  std::ofstream ofs(filename.c_str(), std::ios::out | std::ios::binary);
  if (!ofs.is_open()) {
    NBLA_LOG_WARN("Error in opening file {}", filename);
    return false;
  }

  save_parameters_pb_to_stream(pv, &ofs);
  NBLA_LOG_INFO("Saved parameters to {}", filename);
  return true;
}

// ----------------------------------------------------------------------
// generate a random string for temporary file name.
// Since tmpnam is not safe, and mkstemp is not what I want.
// ----------------------------------------------------------------------
nbla::string random_string(void) {
  int random_string_len = 10;

  static auto &stuff = "0123456789_"
                       "abcdefghijklmnopqrstuvwxy"
                       "ABCDEFGHIJKLMNOPQRSTUVWXY"; // No 'Z'
  thread_local static std::uniform_int_distribution<std::string::size_type>
      pick(0, sizeof(stuff) - 2);
  thread_local static std::mt19937 rg{std::random_device{}()};
  nbla::string random_str;
  random_str.resize(random_string_len);
  for (int i = 0; i < random_string_len; ++i)
    random_str[i] = stuff[pick(rg)];
  return random_str;
}

// ----------------------------------------------------------------------
// save parameters to a .h5 file specified by filename
// ----------------------------------------------------------------------
bool save_parameters_h5(const ParameterVector &pv, char *buffer,
                        unsigned int &size) {
#ifdef NBLA_UTILS_WITH_HDF5
  hid_t file_id;
  nbla::string filename;
  const char *tmp = getenv("TMPDIR");

  H5Eset_auto1(NULL, NULL);
  if (tmp == 0)
    filename = "/tmp";
  else
    filename = tmp;
  filename += "/";
  filename += random_string();
  file_id =
      H5Fcreate(filename.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  create_h5_dataset(pv, file_id);
  H5Fclose(file_id);
  std::ifstream ifs(filename, ios::binary);
  const auto begin = ifs.tellg();
  ifs.seekg(0, ios::end);
  const auto end = ifs.tellg();
  const auto file_size = end - begin;
  if (buffer == NULL) {
    size = file_size;
    remove(filename.c_str());
    return true;
  } else {
    if (size < file_size) {
      NBLA_ERROR(error_code::memory,
                 "Required memory size %d is not satisfied by %d!", file_size,
                 size);
    }
    ifs.seekg(0, ios::beg);
    ifs.read(buffer, file_size);
    size = file_size;
  }
  remove(filename.c_str());
  return true;
#else
  // Not implemented
  NBLA_ERROR(error_code::not_implemented,
             "Cannot write dataset to .h5. HDF5 might not enabled when build.");
  return false;
#endif
}

// ----------------------------------------------------------------------
// save parameters to a .protobuf file specified by filename
// ----------------------------------------------------------------------
bool save_parameters_pb(const ParameterVector &pv, char *buffer,
                        unsigned int &size) {
  std::stringstream ss;

  save_parameters_pb_to_stream(pv, &ss);
  ss.seekp(0, std::ios::end);
  if (buffer == NULL) {
    size = ss.tellp();
  } else {
    if (size < ss.tellp()) {
      NBLA_ERROR(error_code::memory,
                 "Required buffer size %d is not satisfied by %d", ss.tellp(),
                 size)
    }
    size = ss.tellp();
    ss.seekp(0, std::ios::beg);
    ss.read(buffer, size);
  }

  NBLA_LOG_INFO("Saved parameters to buffer!");
  return true;
}

// ----------------------------------------------------------------------
// save parameters to a file specified by filename
// ----------------------------------------------------------------------
bool save_parameters(const ParameterVector &pv, string filename) {
  bool ret = false;
  string ext = get_extension(filename);
  if (ext == ".h5") {
    ret = save_parameters_h5(pv, filename);
  } else if (ext == ".protobuf") {
    ret = save_parameters_pb(pv, filename);
  } else {
    NBLA_ERROR(error_code::value, "Not supported file extension: %s",
               filename.c_str());
  }
  return ret;
}

// ----------------------------------------------------------------------
// Save parameters to file
// ----------------------------------------------------------------------
bool save_parameters(ParameterDirectory &pd, string filename) {
  ParameterVector pv;
  bool ret = false;
  auto pd_param = pd.get_parameters();

  for (auto it = pd_param.begin(); it != pd_param.end(); ++it) {
    pv.push_back({it->first, pd.get_parameter(it->first)});
  }

  ret = save_parameters(pv, filename);
  if (!ret) {
    NBLA_ERROR(error_code::value, "Cannot save parameter file:%s",
               filename.c_str());
    return false;
  }
  NBLA_LOG_INFO("Saved parameters to {}", filename);
  return true;
}

// ----------------------------------------------------------------------
// Save parameters from h5 file buffer
// ----------------------------------------------------------------------
bool save_parameters_h5(ParameterDirectory &pd, char *buf, unsigned int &size) {
  ParameterVector pv;
  auto pd_param = pd.get_parameters();

  for (auto it = pd_param.begin(); it != pd_param.end(); ++it) {
    pv.push_back({it->first, pd.get_parameter(it->first)});
  }

  return save_parameters_h5(pv, buf, size);
}

// ----------------------------------------------------------------------
// Save parameters from pb file buffer
// ----------------------------------------------------------------------
bool save_parameters_pb(ParameterDirectory &pd, char *buf, unsigned int &size) {
  ParameterVector pv;
  auto pd_param = pd.get_parameters();

  for (auto it = pd_param.begin(); it != pd_param.end(); ++it) {
    pv.push_back({it->first, pd.get_parameter(it->first)});
  }

  return save_parameters_pb(pv, buf, size);
}
}
}
