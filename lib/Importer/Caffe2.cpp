// Copyright 2017 Facebook Inc.  All Rights Reserved.

#include "glow/Importer/Caffe2.h"
#include "glow/Base/Tensor.h"
#include "glow/Graph/Graph.h"
#include "glow/Graph/Nodes.h"

#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include "caffe.pb.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace glow;
using llvm::cast;

static auto NCHW2NHWC = {0u, 2u, 3u, 1u};
static auto NHWC2NCHW = {0u, 3u, 1u, 2u};

using ArgumentDictionaryTy =
    std::unordered_map<std::string, const caffe2::Argument *>;

/// Prints a single serialized protocol buffer node. This method is useful for
/// debugging the network and printing errors.
template <typename T>
void unexpectedNodeError(const T &node, const std::string &message) {
  std::string str;
  google::protobuf::TextFormat::PrintToString(node, &str);
  llvm::outs() << message << "\n" << str << "\n";
}

/// Reads a single integer.
static int loadInt(const caffe2::Argument *arg) {
  assert(arg->has_i() && "Node has no Int value");
  return arg->i();
}

/// Reads a single float.
static float loadFloat(const caffe2::Argument *arg) {
  assert(arg->has_f() && "Node has no float value");
  return arg->f();
}

/// Reads a single string.
static const std::string &loadStr(const caffe2::Argument *arg) {
  assert(arg->has_s() && "Node has no str value");
  return arg->s();
}

/// Load the 'shape' record into a vector of sizes.
std::vector<size_t> getShape(const ::caffe2::Argument *arg) {
  std::vector<size_t> dim;
  for (auto i : arg->ints()) {
    dim.push_back(i);
  }
  return dim;
}

/// Translates the protocol buffer node \p op into a random access map.
static ArgumentDictionaryTy loadArgumentMap(const caffe2::OperatorDef &op) {
  ArgumentDictionaryTy dict;
  for (auto i = 0, e = op.arg_size(); i < e; i++) {
    const caffe2::Argument &arg = op.arg(i);
    dict[arg.name()] = &arg;
  }
  return dict;
}

bool caffe2ModelLoader::loadProtoFile(caffe2::NetDef &net,
                                      const std::string &filename) {
  std::ifstream ff(filename, std::ios::in | std::ios::binary);
  GLOW_ASSERT(ff && "Can't find the model or network files.");

  bool parseNet = false;
  if (filename.find(".pbtxt") != std::string::npos) {
    std::string str((std::istreambuf_iterator<char>(ff)),
                    std::istreambuf_iterator<char>());
    parseNet = google::protobuf::TextFormat::ParseFromString(str, &net);
  } else {
    // Construct and configure a Coded Input Stream
    google::protobuf::io::IstreamInputStream filestr(&ff);
    google::protobuf::io::CodedInputStream codedstr(&filestr);
    // Don't warn about large file sizes.
    codedstr.SetTotalBytesLimit(1e+9, 1e+9);
    parseNet = net.ParseFromCodedStream(&codedstr);
  }

  GLOW_ASSERT(parseNet && "Failed to parse the network descriptor.");
  return true;
}

Tensor *caffe2ModelLoader::getTensorByName(const std::string &name) {
  assert(tensors_.count(name) &&
         "There is no tensor registered with this name.");
  return tensors_[name];
}

Node *caffe2ModelLoader::getNodeByName(const std::string &name) {
  auto it = nodeByName_.find(name);
  if (it != nodeByName_.end()) {
    return it->second;
  }

  llvm_unreachable("Could not find a node with this name.");
}

Node *caffe2ModelLoader::getOrCreateNodeByName(const std::string &name) {
  auto it = nodeByName_.find(name);
  if (it != nodeByName_.end()) {
    return it->second;
  }

  Tensor *T = getTensorByName(name);
  auto *V = G_.createVariable(T->getElementType(), T->dims(), name,
                              Variable::VisibilityKind::Private,
                              Variable::TrainKind::Broadcast);
  V->copyFrom(T);
  nodeByName_[name] = V;
  return V;
}

bool caffe2ModelLoader::hasNodeByName(const std::string &name) const {
  auto it = nodeByName_.find(name);
  return (it != nodeByName_.end());
}

void caffe2ModelLoader::loadIntrinsicWeight(const caffe2::OperatorDef &op) {
  assert(op.input().size() == 0 && "Unknown weights do not support inputs.");

  std::vector<TypeRef> outputs;
  for (const auto &output : op.output()) {
    auto *T = new Tensor();
    outputs.emplace_back(G_.getVoidTy());
    tensors_[output] = T;
  }

  Node *node = G_.createIntrinsicNode(op.name(), "caffe2.opaque", {}, outputs,
                                      (void *)&op);
  for (int i = 0, e = op.output_size(); i < e; i++) {
    nodeByName_[op.output(i)] = node;
  }
}

void caffe2ModelLoader::loadIntrinsicOperator(const caffe2::OperatorDef &op) {
  std::vector<Node *> inputs;
  for (const auto &input : op.input()) {
    inputs.emplace_back(getOrCreateNodeByName(input));
  }
  std::vector<TypeRef> outputs;
  for (int i = 0; i < op.output().size(); ++i) {
    outputs.emplace_back(G_.getVoidTy());
  }

  Node *node = G_.createIntrinsicNode(op.name(), "caffe2.opaque",
                                      llvm::ArrayRef<Node *>(inputs), outputs,
                                      (void *)&op);
  for (int i = 0, e = op.output_size(); i < e; i++) {
    nodeByName_[op.output(i)] = node;
  }
}

void caffe2ModelLoader::loadOperator(const caffe2::OperatorDef &op) {
  ArgumentDictionaryTy dict = loadArgumentMap(op);

  const std::string &typeName = op.type();

  if (typeName == "Relu") {
    // Load the inputs:
    auto *in = getOrCreateNodeByName(op.input(0));
    // Create the RELU:
    auto *R = G_.createRELU(op.name(), in);
    // Save the outputs:
    for (int i = 0, e = op.output_size(); i < e; i++) {
      nodeByName_[op.output(i)] = R;
    }
    return;
  }

  if (typeName == "Conv") {
    // Load the inputs:
    int stride = loadInt(dict["stride"]);
    int pad = dict.count("pad") ? loadInt(dict["pad"]) : 0;
    int kernel = loadInt(dict["kernel"]);

    auto *in = getOrCreateNodeByName(op.input(0));
    Tensor *w = getTensorByName(op.input(1));

    // Transpose the weights to the right format. Glow expects to read the
    // weights in the format CRSK. Caffe2 stores the operators as KCRS.
    // C - output_depth, R - filter_height, S - filter_width, K - input_depth.

    // TODO: need to test this code with a large batch size to verify that the
    // conversion is correct.
    Tensor wtag;
    w->getHandle<>().transpose(&wtag, {0, 2, 3, 1});

    // The structure of the conv weigts is: NHWC. We take the C, which is the
    // number of filters. We use this value to calculate the size of the bias
    // if it is not specified.
    size_t numFilters = wtag.dims()[0];

    // Load the bias vector:
    Tensor *b = nullptr;

    // Check if we have a serialized bias vector.
    if (op.input_size() > 2) {
      auto &biasTensorName = op.input(2);
      if (tensors_.count(biasTensorName)) {
        b = getTensorByName(biasTensorName);
      }
    }

    auto *tr = G_.createTranspose(op.name(), in, NCHW2NHWC);
    auto *node = G_.createConv(op.name(), tr, numFilters, kernel, stride, pad);

    cast<Variable>(node->getFilter())->copyFrom(&wtag);

    // If we don't have a bias vector then create one that matches the weight
    // size and fill it with zeros.
    if (b) {
      cast<Variable>(node->getBias())->copyFrom(b);
    } else {
      cast<Variable>(node->getBias())->getPayload().zero();
    }

    auto *N = G_.createTranspose(op.name(), node, NHWC2NCHW);
    // Save the outputs:
    for (int i = 0, e = op.output_size(); i < e; i++) {
      nodeByName_[op.output(i)] = N;
    }
    return;
  }

  if (typeName == "MaxPool" || typeName == "AveragePool") {
    using OpKind = PoolNode::Mode;
    OpKind opk = (typeName == "MaxPool") ? OpKind::Max : OpKind::Avg;

    // Load the inputs:
    auto *in = getOrCreateNodeByName(op.input(0));
    int stride = loadInt(dict["stride"]);
    int pad = dict.count("pad") ? loadInt(dict["pad"]) : 0;
    size_t kernel = loadInt(dict["kernel"]);

    auto *tr = G_.createTranspose(op.name(), in, NCHW2NHWC);

    // If 'global_pooling' is set then the operation will pool over the size of
    // the input by doing: kernel = height/width.
    if (dict.count("global_pooling")) {
      auto Ty = in->getType();
      kernel = Ty->dims()[3];
    }

    auto *node = G_.createPool(op.name(), tr, opk, kernel, stride, pad);
    auto *N = G_.createTranspose(op.name(), node, NHWC2NCHW);

    // Save the outputs:
    for (int i = 0, e = op.output_size(); i < e; i++) {
      nodeByName_[op.output(i)] = N;
    }
    return;
  }

  if (typeName == "Dropout") {
    auto *in = getOrCreateNodeByName(op.input(0));
    // Save the identity operation:
    for (int i = 0, e = op.output_size(); i < e; i++) {
      nodeByName_[op.output(i)] = in;
    }
    return;
  }

  if (typeName == "SpatialBN") {
    auto *in = getOrCreateNodeByName(op.input(0));
    auto *scale = getTensorByName(op.input(1));
    auto *bias = getTensorByName(op.input(2));
    auto *mean = getTensorByName(op.input(3));
    auto *var = getTensorByName(op.input(4));
    float epsilon = 1e-5f; // default
    auto epsilonIt = dict.find("epsilon");
    if (epsilonIt != dict.end()) {
      epsilon = loadFloat(epsilonIt->second);
    }

    unsigned channel = 0;
    std::string order = "NCHW"; // default
    auto orderIt = dict.find("order");
    if (orderIt != dict.end()) {
      order = loadStr(orderIt->second);
    }
    if (order == "NHWC") {
      channel = 3;
    } else if (order == "NCHW") {
      channel = 1;
    } else {
      GLOW_ASSERT(false && "Invalid order field");
    }

    auto *node = G_.createBatchNormalization(op.name(), in, channel, epsilon);

    // Load the weights.
    cast<Variable>(node->getScale())->copyFrom(scale);
    cast<Variable>(node->getBias())->copyFrom(bias);
    cast<Variable>(node->getMean())->copyFrom(mean);
    cast<Variable>(node->getVar())->copyFrom(var);

    for (int i = 0, e = op.output_size(); i < e; i++) {
      nodeByName_[op.output(i)] = node;
    }
    return;
  }

  if (typeName == "Concat") {
    auto *lhs = getOrCreateNodeByName(op.input(0));
    auto *rhs = getOrCreateNodeByName(op.input(1));

    unsigned channel = 0;
    auto order = loadStr(dict["order"]);
    if (order == "NHWC") {
      channel = 3;
    } else if (order == "NCHW") {
      channel = 1;
    } else {
      GLOW_ASSERT(false && "Invalid order field");
    }

    Node *node = G_.createConcat(op.name(), {lhs, rhs}, channel);

    for (int i = 0, e = op.output_size(); i < e; i++) {
      nodeByName_[op.output(i)] = node;
    }
    return;
  }
  if (typeName == "Sum") {
    auto *in0 = getOrCreateNodeByName(op.input(0));
    auto *in1 = getOrCreateNodeByName(op.input(1));
    auto *node =
        G_.createArithmetic(op.name(), in0, in1, ArithmeticNode::Mode::Add);
    // Save the outputs:
    for (int i = 0, e = op.output_size(); i < e; i++) {
      nodeByName_[op.output(i)] = node;
    }
    return;
  }

  if (typeName == "Softmax") {
    auto *softmaxExpected = getOrCreateNodeByName("softmax_expected");

    // Load the inputs:
    Node *in = getOrCreateNodeByName(op.input(0));

    // Caffe2 allows shapes like <N x 10 x 1 x 1 >. Flatten the inputs to the
    // softmax function. This is similar to a bitcast operation.
    auto flatten = flattenCdr(in->getType()->dims());
    in = G_.createReshape("reshape", in, {flatten.first, flatten.second});

    auto *node = G_.createSoftMax(op.name(), in, softmaxExpected);
    // Save the outputs:
    for (int i = 0, e = op.output_size(); i < e; i++) {
      nodeByName_[op.output(i)] = node;
    }
    return;
  }
  if (typeName == "FC") {
    // Load the inputs:
    auto *in = getOrCreateNodeByName(op.input(0));
    // Load weights.
    Tensor *w = getTensorByName(op.input(1));
    Tensor *b = getTensorByName(op.input(2));
    auto FCdepth = b->size();

    // Caffe2 stores the transposed W matrix. In here we transpose W back.
    Tensor wtag;
    w->getHandle<>().transpose(&wtag, {1, 0});

    auto W = G_.addVar(new Variable("weights", std::move(wtag)));
    auto B = G_.addVar(new Variable("biases", std::move(*b)));
    auto *FC = G_.createFullyConnected(op.name(), in, W, B, FCdepth);

    // Save the outputs:
    for (int i = 0, e = op.output_size(); i < e; i++) {
      nodeByName_[op.output(i)] = FC;
    }
    return;
  }
  if (typeName == "LRN") {
    auto *in = getOrCreateNodeByName(op.input(0));

    size_t size = loadInt(dict["size"]);
    float alpha = loadFloat(dict["alpha"]);
    float beta = loadFloat(dict["beta"]);
    float k = loadFloat(dict["bias"]);

    auto *tr = G_.createTranspose(op.name(), in, NCHW2NHWC);

    auto *node = G_.createLocalResponseNormalization(op.name(), tr, size / 2,
                                                     alpha, beta, k);

    auto *N = G_.createTranspose(op.name(), node, NHWC2NCHW);

    // Save the outputs:
    for (int i = 0, e = op.output_size(); i < e; i++) {
      nodeByName_[op.output(i)] = N;
    }
    return;
  }

  loadIntrinsicOperator(op);
}

void caffe2ModelLoader::loadNetwork(caffe2::NetDef &net) {
  /// Load the network operators:
  for (int i = 0; i < net.op_size(); i++) {
    auto &op = net.op(i);
    loadOperator(op);
  }

  assert(net.external_output_size() &&
         "Network needs external outputs defined.");
  auto *r = getNodeByName(net.external_output(0));
  root_ = G_.createSave("output", r);
}

void caffe2ModelLoader::loadWeights(caffe2::NetDef &net) {
  for (auto &op : net.op()) {
    ArgumentDictionaryTy dict = loadArgumentMap(op);

    /// Load tensors with values:
    if (op.type() == "GivenTensorFill") {
      /*
       output: "conv1_w"
       name: ""
       type: "GivenTensorFill"
       arg {
       name: "shape"
       ints: 96
       ints: 3
       ints: 11
       ints: 11
       }
       arg {
       name: "values"
       floats: -0.028315347
       */

      auto *T = new Tensor();
      for (auto &o : op.output()) {
        tensors_[o] = T;
      }

      auto dim = getShape(dict["shape"]);
      T->reset(ElemKind::FloatTy, dim);
      auto TH = T->getHandle<>();
      size_t i = 0;
      for (auto f : dict["values"]->floats()) {
        TH.raw(i++) = f;
      }

      assert(i == TH.size() && "The number of serialized values does not "
                               "match the size of the tensor.");
      continue;
    }

    // Load tensors with constant fill:
    if (op.type() == "ConstantFill") {
      /*
       output: "data"
       name: ""
       type: "ConstantFill"
       arg {
       name: "shape"
       ints: 1
       }
       */

      const auto &name = op.output(0);
      // If the tensor is pre-populated by the user of this class then we don't
      // need to allocate a new tensor.
      if (tensors_.count(name)) {
        continue;
      }

      auto *T = new Tensor();
      tensors_[name] = T;

      auto dim = getShape(dict["shape"]);
      T->reset(ElemKind::FloatTy, dim);
      auto TH = T->getHandle<>();
      TH.clear();
      continue;
    }

    loadIntrinsicWeight(op);
  }
}

caffe2ModelLoader::caffe2ModelLoader(const std::string &netDescFilename,
                                     const std::string &netWeightFilename,
                                     llvm::ArrayRef<const char *> names,
                                     llvm::ArrayRef<Tensor *> tensors, Graph &G)
    : G_(G) {
  // Verify that the version of the library that we linked against is
  // compatible with the version of the headers we compiled against.
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // The caffe2 weights that we are deserializing.
  caffe2::NetDef weightsDef;
  // The caffe2 network descriptor that we are deserializing.
  caffe2::NetDef networkDef;

  assert(names.size() == tensors.size() && "Invalid initialization list");
  for (unsigned i = 0; i < names.size(); i++) {
    auto *T = tensors[i];
    auto *V = G_.createVariable(T->getElementType(), T->dims(), names[i],
                                Variable::VisibilityKind::Public,
                                Variable::TrainKind::None);
    V->copyFrom(T);
    nodeByName_[names[i]] = V;
  }

  loadProtoFile(networkDef, netDescFilename);
  loadProtoFile(weightsDef, netWeightFilename);
  loadWeights(weightsDef);
  loadNetwork(networkDef);
}

caffe2ModelLoader::~caffe2ModelLoader() {
  for (auto it : tensors_) {
    delete it.second;
  }
}
