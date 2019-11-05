// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "core/optimizer/initializer.h"
#include "core/optimizer/layer_norm_fusion.h"
#include "core/graph/graph_utils.h"
#include "float.h"
#include <deque>

using namespace ONNX_NAMESPACE;
using namespace onnxruntime::common;
namespace onnxruntime {

static const Node* first_child_by_type(Node& node, std::string child_type) {
  //const Node* p_child_found = nullptr;
  for (auto it = node.OutputNodesBegin(); it != node.OutputNodesEnd(); ++it) {
    std::cout << "children optype " << (*it).OpType() << "\n";
    if ((*it).OpType().compare(child_type) == 0) {
      //p_child_found = &(*it);
      std::cout << "Matched node found! \n";
      //break;
      return &(*it);
    }
  }

  return nullptr;
}

static const Node* first_parent_by_type(Node& node, std::string parent_type) {
  //const Node* p_child_found = nullptr;
  for (auto it = node.InputNodesBegin(); it != node.InputNodesEnd(); ++it) {
    std::cout << "parent optype " << (*it).OpType() << "\n";
    if ((*it).OpType().compare(parent_type) == 0) {
      //p_child_found = &(*it);
      std::cout << "Matched node found! \n";
      //break;
      return &(*it);
    }
  }

  return nullptr;
}

// Gelu supports limited data types.
static std::vector<std::string> supported_data_types{"tensor(float16)", "tensor(float)", "tensor(double)"};

static bool IsSupportedDataType(const Node& node) {
  for (const auto& input_arg : node.InputDefs()) {
    std::cout << *(input_arg->Type()) << "\n";
    if (std::find(supported_data_types.begin(), supported_data_types.end(),
                  *(input_arg->Type())) == supported_data_types.end()) {
      return false;
    }
  }
  return true;
}

Status LayerNormFusion::ApplyImpl(Graph& graph, bool& modified, int graph_level) const {
  GraphViewer graph_viewer(graph);
  const auto& node_topology_list = graph_viewer.GetNodesInTopologicalOrder();
  std::vector<std::reference_wrapper<Node>> nodes_to_remove;
  for (auto node_index : node_topology_list) {
    nodes_to_remove.clear();
    std::cout << "entering fusion\n";
    auto* p_reduce_mean = graph.GetNode(node_index);
    if (p_reduce_mean == nullptr)
      continue;  // we removed the node as part of an earlier fusion
    Node& reduce_mean_node = *p_reduce_mean;
    ORT_RETURN_IF_ERROR(Recurse(reduce_mean_node, modified, graph_level));
    std::cout << reduce_mean_node.OpType() << "\n";
    std::cout << graph_utils::IsSupportedOptypeVersionAndDomain(reduce_mean_node, "ReduceMean", {1});
    std::cout << "node.op since version " << reduce_mean_node.Op()->SinceVersion() << "\n";

    if (!graph_utils::IsSupportedOptypeVersionAndDomain(reduce_mean_node, "ReduceMean", {1}) ||
        !graph_utils::IsSupportedProvider(reduce_mean_node, GetCompatibleExecutionProviders()) ||
        (reduce_mean_node.GetOutputEdgesCount() != 1 && reduce_mean_node.GetOutputEdgesCount() != 2) ||
        !IsSupportedDataType(reduce_mean_node)) {  // TODO: Is there any type restriction?
      continue;
    }
    nodes_to_remove.push_back(reduce_mean_node);

    // Loop through the children of current "ReduceMean" node. See if they match ["Sub"] or ["Sub", "Sub"]
    int subCnt = 0;
    const Node* p_sub_node = nullptr;
    const Node* p_sub_node_dup = nullptr;
    for (auto iter = reduce_mean_node.OutputNodesBegin(); iter != reduce_mean_node.OutputNodesEnd(); ++iter) {
      if ((*iter).OpType().compare("Sub") == 0) {
        if (subCnt == 0) {
          p_sub_node = &(*iter);

        } else {
          p_sub_node_dup = &(*iter);
        }
        subCnt++;
      } else {
        // doesn't match layer norm pattern. break.
        subCnt = -1;
        break;
      }
    }

    if (subCnt != 1 && subCnt != 2) {
      continue;
    }
    Node& sub_node = *graph.GetNode(p_sub_node->Index());
    if (!graph_utils::IsSupportedOptypeVersionAndDomain(sub_node, "Sub", {7}) ||
        sub_node.GetExecutionProviderType() != reduce_mean_node.GetExecutionProviderType() ||
        !IsSupportedDataType(sub_node)) {
      continue;
    }
    nodes_to_remove.push_back(sub_node);
    std::cout << "sub found\n";
    const Node* p_div = nullptr;
    p_div = first_child_by_type(sub_node, "Div");
    if (p_div == nullptr) {
      std::cout << "sub_node has not p_div. Check if sub node has a dup. \n";
      // Find the sub_dup node if exist
      if (p_sub_node_dup != nullptr) {
        Node& sub_node_dup = *graph.GetNode(p_sub_node_dup->Index());
        nodes_to_remove.push_back(sub_node_dup);
        if (!graph_utils::IsSupportedOptypeVersionAndDomain(sub_node_dup, "Sub", {7}) ||
            sub_node_dup.GetExecutionProviderType() != reduce_mean_node.GetExecutionProviderType() ||
            sub_node_dup.GetOutputEdgesCount() != 1 ||
            !IsSupportedDataType(sub_node_dup)) {
          continue;
        }
        std::cout << "sub dup found\n";
        p_div = first_child_by_type(sub_node_dup, "Div");
      } else {
        std::cout << "sub dup not found. exit.\n";
        continue;
      }
    }

    if (p_div == nullptr) {
      std::cout << "no div from two subs. exit.\n ";
      continue;
    }
    std::cout << "get div node.\n";
    Node& div_node = *graph.GetNode(p_div->Index());
    std::cout << "div here. Checking.\n ";
    if (!graph_utils::IsSupportedOptypeVersionAndDomain(div_node, "Div", {7}) ||
        div_node.GetExecutionProviderType() != reduce_mean_node.GetExecutionProviderType() ||
        div_node.GetOutputEdgesCount() != 1 ||
        !IsSupportedDataType(div_node)) {
      std::cout << "div check failed. \n";
      continue;
    }
    nodes_to_remove.push_back(div_node);
    std::cout << "div found\n";

    // Traceback the div node to see if sqrt --> div
    const Node* p_sqrt = first_parent_by_type(div_node, "Sqrt");
    if (p_sqrt == nullptr) {
      std::cout << "no sqrt found \n";
      continue;
    }
    Node& sqrt_node = *graph.GetNode(p_sqrt->Index());
    std::cout << sqrt_node.OpType() << "\n";
    std::cout << graph_utils::IsSupportedOptypeVersionAndDomain(sqrt_node, "Sqrt", {6});
    std::cout << "node.op since version " << sqrt_node.Op()->SinceVersion() << "\n";

    std::cout << IsSupportedDataType(sqrt_node);
    if (!graph_utils::IsSupportedOptypeVersionAndDomain(sqrt_node, "Sqrt", {6}) ||
        sqrt_node.GetExecutionProviderType() != reduce_mean_node.GetExecutionProviderType() ||
        sqrt_node.GetOutputEdgesCount() != 1 ||
        !IsSupportedDataType(sqrt_node)) {
      std::cout << "sqrt check failed. \n";
      continue;
    }
    nodes_to_remove.push_back(sqrt_node);
    std::cout << "sqrt found\n";

    // add --> sqrt
    Node& add2_node = *graph.GetNode(sqrt_node.InputNodesBegin()->Index());
    std::cout << "Matches Op " << graph_utils::MatchesOpSinceVersion(add2_node, {7}) << "\n";
    std::cout << "node.op since version " << add2_node.Op()->SinceVersion() << "\n";
    if (!graph_utils::IsSupportedOptypeVersionAndDomain(add2_node, "Add", {7}) ||
        add2_node.GetExecutionProviderType() != reduce_mean_node.GetExecutionProviderType() ||
        add2_node.GetOutputEdgesCount() != 1 ||
        !IsSupportedDataType(add2_node)) {
      continue;
    }
    nodes_to_remove.push_back(add2_node);
    std::cout << "add2 found\n";

    // reduceMean --> add
    const Node* p_reduce_mean2 = nullptr;

    p_reduce_mean2 = first_parent_by_type(add2_node, "ReduceMean");
    Node& reduce_mean2_node = *graph.GetNode(p_reduce_mean2->Index());
    std::cout << "Matches Op " << graph_utils::MatchesOpSinceVersion(reduce_mean2_node, {1}) << "\n";
    std::cout << "node.op since version " << reduce_mean2_node.Op()->SinceVersion() << "\n";
    if (!graph_utils::IsSupportedOptypeVersionAndDomain(reduce_mean2_node, "ReduceMean", {1}) ||
        reduce_mean2_node.GetExecutionProviderType() != reduce_mean_node.GetExecutionProviderType() ||
        reduce_mean2_node.GetOutputEdgesCount() != 1 ||
        !IsSupportedDataType(reduce_mean2_node)) {
      continue;
    }
    nodes_to_remove.push_back(reduce_mean2_node);
    std::cout << "reduce mean found\n";

    // pow --> reduceMean
    Node& pow_node = *graph.GetNode(reduce_mean2_node.InputNodesBegin()->Index());
    if (!graph_utils::IsSupportedOptypeVersionAndDomain(pow_node, "Pow", {7}) ||
        pow_node.GetExecutionProviderType() != reduce_mean_node.GetExecutionProviderType() ||
        pow_node.GetOutputEdgesCount() != 1 ||
        !IsSupportedDataType(pow_node)) {
      continue;
    }
    nodes_to_remove.push_back(pow_node);
    std::cout << "pow found";

    // sub --> pow
    const Node* p_sub2_node = first_parent_by_type(pow_node, "Sub");
    Node& sub2_node = *graph.GetNode(p_sub2_node->Index());
    if (!graph_utils::IsSupportedOptypeVersionAndDomain(sub2_node, "Sub", {7}) ||
        sub2_node.GetExecutionProviderType() != reduce_mean_node.GetExecutionProviderType() ||
        !IsSupportedDataType(sub2_node)) {
      continue;
    }
    nodes_to_remove.push_back(sub2_node);
    std::cout << "sub2 found\n";

    // add --> sub
    const Node* p_reduce_mean_check = first_parent_by_type(sub2_node, "ReduceMean");
    if (p_reduce_mean_check == nullptr || p_reduce_mean_check != &reduce_mean_node) {
      continue;
    }
    std::cout << "is the same add!!!";

    // div --> mul
    Node& mul_node = *graph.GetNode(div_node.OutputNodesBegin()->Index());
    if (!graph_utils::IsSupportedOptypeVersionAndDomain(mul_node, "Mul", {7}) ||
        mul_node.GetExecutionProviderType() != reduce_mean_node.GetExecutionProviderType() ||
        !IsSupportedDataType(mul_node)) {
      std::cout << "mul check failed. Exit. \n";
      continue;
    }
    nodes_to_remove.push_back(mul_node);
    std::cout << "Mul found (last).\n";

    // mul --> add
    Node& last_add_node = *graph.GetNode(mul_node.OutputNodesBegin()->Index());
    if (!graph_utils::IsSupportedOptypeVersionAndDomain(last_add_node, "Add", {7}) ||
        last_add_node.GetExecutionProviderType() != reduce_mean_node.GetExecutionProviderType() ||
        !IsSupportedDataType(last_add_node)) {
      std::cout << "add check failed. Exit. \n";
      continue;
    }
    nodes_to_remove.push_back(last_add_node);
    std::cout << "Add found (last).\n";

	// Get the inputs for the new layernorm node
	NodeArg* scale = nullptr;
    NodeArg* bias = nullptr;
    for (int i = 0; i < mul_node.MutableInputDefs().size(); i++) {
	  if (graph_utils::NodeArgIsConstant(graph, *(mul_node.MutableInputDefs()[i])) || 
		  graph_utils::IsGraphInput(graph, mul_node.MutableInputDefs()[i])) {
        scale = mul_node.MutableInputDefs()[i];
	  }
	}

	for (int i = 0; i < last_add_node.MutableInputDefs().size(); i++) {
      if (graph_utils::NodeArgIsConstant(graph, *(last_add_node.MutableInputDefs()[i])) ||
		  graph_utils::IsGraphInput(graph, last_add_node.MutableInputDefs()[i])) {
        bias = mul_node.MutableInputDefs()[i];
	  }
    }
    if (scale == nullptr || bias == nullptr) {
      std::cout << "Cannot find inputs for the new node\n";
		continue;
	}
    const std::vector<NodeArg*> layer_norm_input_defs{reduce_mean_node.MutableInputDefs()[0],
                                                    scale,
                                                    bias};
    Node& layer_norm_node = graph.AddNode(graph.GenerateNodeName("LayerNormalization"),
                                          "LayerNormalization",
                                          "fused LayerNorm subgraphs ",
                                          layer_norm_input_defs,
                                          {}, {}, kOnnxDomain);

    // Assign provider to this new node. Provider should be same as the provider for old node.
    layer_norm_node.SetExecutionProviderType(reduce_mean_node.GetExecutionProviderType());

    // move input edges to add (first in list) across to the layer_norm_node.
    // move output definitions and output edges from mul_node (last in list) to layer_norm_node.
    // remove all the other nodes.
    graph_utils::FinalizeNodeFusion(graph, nodes_to_remove, layer_norm_node);
    std::cout << "fusion done\n";

    modified = true;
  }
  std::cout << "Loop done. \n";
  return Status::OK();
}
}  // namespace onnxruntime