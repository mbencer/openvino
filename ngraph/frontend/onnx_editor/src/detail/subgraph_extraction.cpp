// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <functional>
#include <onnx/onnx_pb.h>
#include <stack>

#include "ngraph/check.hpp"
#include "onnx_editor/detail/subgraph_extraction.hpp"

using namespace ngraph::onnx_editor;

namespace
{
    void validate_node_index(const ONNX_NAMESPACE::GraphProto& graph, const int node_idx)
    {
        NGRAPH_CHECK(
            node_idx >= 0 && node_idx < graph.node_size(),
            "The specified node index is out of range of nodes in the original model(idx: ",
            std::to_string(node_idx),
            "; nodes count in the model: ",
            std::to_string(graph.node_size()),
            ")");
    }

    template <typename T>
    std::function<bool(const T&)> name_equals(const std::string& name)
    {
        return [&name](const T& onnx_object) -> bool { return onnx_object.name() == name; };
    }

    const auto is_equal_to =
        +[](const std::string& other) { return [&](const std::string& s) { return s == other; }; };

    /// \brief Checks if an item with name equal to "name" already exists in the specified
    ///        container. A container item is expected to have a name() method.
    template <typename Container>
    bool already_exists(const Container& items, const std::string& name)
    {
        using std::begin;
        using std::end;
        return std::any_of(
            begin(items), end(items), name_equals<typename Container::value_type>(name));
    }

    /// \brief Checks if a tensor with name "name" is produced by an input of the graph
    bool is_graph_input(const ONNX_NAMESPACE::GraphProto& graph, const std::string& name)
    {
        return already_exists(graph.input(), name);
    }

    /// \brief Checks if a tensor with name "name" is produced by an initializer of the graph
    bool is_graph_initializer(const ONNX_NAMESPACE::GraphProto& graph, const std::string& name)
    {
        return already_exists(graph.initializer(), name);
    }

    /// \brief Looks up the index of a node that produces a tensor "input_name". Used to traverse
    ///        the graph bottom-up. Starts from a node index "current_node_idx" because it operates
    ///        on a topologically sorted graph.
    int find_source_node_idx(const ONNX_NAMESPACE::GraphProto& graph,
                             const int current_node_idx,
                             const std::string& input_name)
    {
        for (int i = current_node_idx - 1; i >= 0; --i)
        {
            const auto& outputs = graph.node(i).output();
            const auto output_found =
                std::any_of(std::begin(outputs), std::end(outputs), is_equal_to(input_name));

            if (output_found)
            {
                return i;
            }
        }

        throw ngraph::ngraph_error{
            "Source node not found in the graph for node: " + std::to_string(current_node_idx) +
            " and input name: " + input_name};
    }

    /// \brief Looks up a descriptor for a given tensor name. This descriptor contains inferred
    ///        shape information which is required to create new inputs and outputs in the graph.
    const ONNX_NAMESPACE::ValueInfoProto&
        find_tensor_descriptor(const ONNX_NAMESPACE::GraphProto& graph,
                               const std::string& tensor_name)
    {
        const auto it = std::find_if(std::begin(graph.value_info()),
                                     std::end(graph.value_info()),
                                     name_equals<ONNX_NAMESPACE::ValueInfoProto>(tensor_name));

        NGRAPH_CHECK(it != std::end(graph.value_info()),
                     "Could not find a tensor descriptor for tensor '",
                     tensor_name,
                     "'. It's not possible to add a new input to the graph without the type and "
                     "shape information of the intermediate tensor.");

        return *it;
    }

    std::string get_input_tensor_name(ONNX_NAMESPACE::GraphProto& graph, const InputEdge& edge)
    {
        // TODO: SHOULD BE CHECKED / USE MAPPER?
        return graph.node(edge.m_node_idx).input(edge.m_port_idx);
    }

    std::string get_output_tensor_name(ONNX_NAMESPACE::GraphProto& graph, const OutputEdge& edge)
    {
        // TODO: SHOULD BE CHECKED / USE MAPPER?
        return graph.node(edge.m_node_idx).output(edge.m_port_idx);
    }

    /// \brief Inserts a new input to the graph and removes an initializer that produced a tensor
    ///        specified by an input edge passed to this function.
    void replace_initializer_with_new_input(ONNX_NAMESPACE::GraphProto& graph,
                                            const InputEdge& edge)
    {
        const auto tensor_name = get_input_tensor_name(graph, edge);
        const auto it = std::find_if(std::begin(graph.initializer()),
                                     std::end(graph.initializer()),
                                     name_equals<ONNX_NAMESPACE::TensorProto>(tensor_name));

        NGRAPH_CHECK(it != std::end(graph.initializer()),
                     "Could not find an initializer in the graph: '",
                     tensor_name);

        if (!already_exists(graph.input(), tensor_name))
        {
            const auto& initializer = *it;
            auto& new_input = *(graph.add_input());

            auto& new_input_tensor_type = *(new_input.mutable_type()->mutable_tensor_type());
            new_input_tensor_type.set_elem_type(initializer.data_type());

            auto& new_input_shape = *(new_input_tensor_type.mutable_shape());
            for (const auto initializer_dim : initializer.dims())
            {
                auto& new_dim = *(new_input_shape.add_dim());
                new_dim.set_dim_value(initializer_dim);
            }

            *(new_input.mutable_name()) = tensor_name;
        }

        graph.mutable_initializer()->erase(
            it); // TODO: What if some other node uses this initializer too?
    }

    /// \brief Inserts a new input to the graph and connects it to the node designated by an input
    ///        edge passed to this function.
    /// \return A new input edge (along with "true") if a new input was added to the graph,
    ///         false + the original edge otherwise.
    std::pair<bool, std::string>
        append_new_graph_input(ONNX_NAMESPACE::GraphProto& graph,
                               const InputEdge& edge) // node_index, port_number
    {
        const auto tensor_name = get_input_tensor_name(graph, edge);
        if (already_exists(graph.input(), tensor_name) &&
            !is_graph_initializer(graph, tensor_name)) // todo check
        {
            // no need to append a new input if an edge points to an existing one in the model
            return {false, tensor_name};
        }

        auto& target_node = *(graph.mutable_node(edge.m_node_idx));
        NGRAPH_CHECK(edge.m_port_idx < target_node.input().size(),
                     "Input '",
                     edge.m_port_idx,
                     "' not found in the inputs of node ",
                     edge.m_node_idx,
                     ". Cannot append a new graph input to this node.");

        auto target_input = target_node.mutable_input(edge.m_port_idx);

        const std::string new_input_name = target_node.output(0) + ":" + tensor_name;

        // if an edge is connected to an initializer, the initializer is removed and substituted
        // with an input
        if (is_graph_initializer(graph, tensor_name))
        {
            replace_initializer_with_new_input(graph, edge);
            return {false, tensor_name};
        }
        else
        {
            auto& new_input = *(graph.add_input());
            // copy the intermediate tensor properties to the newly created input
            new_input.MergeFrom(find_tensor_descriptor(graph, tensor_name));
            *(new_input.mutable_name()) = new_input_name;
            // attach the new graph input to the target node's input
            *target_input = new_input_name;
            return {true, tensor_name};
        }
    }

    /// \brief Replaces a node or initializer (consumed by multiple nodes) with a new input
    /// \return Returns an index of a removed node or -1 if an initializer was removed
    int replace_source_with_new_input(ONNX_NAMESPACE::GraphProto& graph, const InputEdge& edge)
    {
        const auto tensor_name = get_input_tensor_name(graph, edge);
        if (already_exists(graph.input(), tensor_name) && !is_graph_initializer(graph, tensor_name))
        {
            // happens when a user specifies multiple input edges pointing to the same tensor name
            return -1;
        }

        if (is_graph_initializer(graph, tensor_name))
        {
            replace_initializer_with_new_input(graph, edge);
        }
        else
        {
            auto& new_input = *(graph.add_input());
            // copy the intermediate tensor properties to the newly created input
            new_input.MergeFrom(find_tensor_descriptor(graph, tensor_name));

            const auto source_node_idx = find_source_node_idx(graph, edge.m_node_idx, tensor_name);
            auto& source_node = *(graph.mutable_node(source_node_idx));
            auto& node_outputs = *source_node.mutable_output();
            auto target_output =
                std::find(std::begin(node_outputs), std::end(node_outputs), tensor_name);

            NGRAPH_CHECK(target_output != std::end(node_outputs),
                         "Output '",
                         tensor_name,
                         "' not found in the outputs of node ",
                         source_node_idx,
                         ". Cannot remove the output from this node.");

            // stop produsing tensor "tensor_name by the source node of the processed edge
            *target_output = "";

            return source_node_idx;
        }

        return -1;
    }

    /// \brief Adds new outputs to the ONNX graph for an edge specified by a user
    /// The shape for this output is taken from a previously executed shape inference of the
    /// original model.
    void append_new_graph_output(ONNX_NAMESPACE::GraphProto& graph, const OutputEdge& edge)
    {
        const auto tensor_name = get_output_tensor_name(graph, edge);
        if (already_exists(graph.output(), tensor_name))
        {
            return;
        }

        auto& target_node = *(graph.mutable_node(edge.m_node_idx));
        const auto& node_outputs = target_node.output();
        const auto target_output =
            std::find(std::begin(node_outputs), std::end(node_outputs), tensor_name);

        NGRAPH_CHECK(target_output != std::end(node_outputs),
                     "Output '",
                     tensor_name,
                     "' not found in the outputs of node ",
                     edge.m_node_idx,
                     ". Cannot append a new graph output to this node.");

        auto& new_output = *(graph.add_output());
        // copy the intermediate tensor's properties to the newly created
        new_output.MergeFrom(find_tensor_descriptor(graph, tensor_name));
        *(new_output.mutable_name()) = tensor_name;
    }

    /// \brief Removes all items from a container except the ones whose names are in items_to_keep
    ///        It's intended to work with ONNX graph inputs, outputs and initializers only.
    template <typename Container>
    void discard_by_name(Container& all_items, const std::set<std::string>& items_to_keep)
    {
        static_assert(
            std::is_same<typename Container::value_type, ONNX_NAMESPACE::ValueInfoProto>::value ||
                std::is_same<typename Container::value_type, ONNX_NAMESPACE::TensorProto>::value,
            "Unsupported value type of the container");

        // The tested item can be discarded if its name is not found in the items_to_keep set
        const auto can_be_discarded = [&items_to_keep](const typename Container::value_type& item) {
            return items_to_keep.count(item.name()) == 0;
        };

        using std::begin;
        using std::end;

        // move the elements-to-discard to the end of the container
        const auto new_end = std::remove_if(begin(all_items), end(all_items), can_be_discarded);
        // erase all of the discarded elements past the new end of the container
        all_items.erase(new_end, end(all_items));
    }

    /// \brief Removes all nodes from a container keeping the ones whose index is in nodes_to_keep
    template <typename Container>
    void discard_nodes(Container& all_nodes, const std::set<int>& nodes_to_keep)
    {
        static_assert(
            std::is_same<typename Container::value_type, ONNX_NAMESPACE::NodeProto>::value,
            "Unsupported value type of the container");

        int idx = 0;
        const auto discard_node = [&idx, &nodes_to_keep](const typename Container::value_type&) {
            return nodes_to_keep.count(idx++) == 0;
        };

        using std::begin;
        using std::end;

        const auto new_end = std::remove_if(begin(all_nodes), end(all_nodes), discard_node);
        all_nodes.erase(new_end, end(all_nodes));
    }
} // namespace

/* -----------------------------------------------------------------------------------------------*/

SubgraphExtractor::SubgraphExtractor(ONNX_NAMESPACE::GraphProto& graph)
    : m_onnx_graph(graph)
    , m_node_inputs(graph.node().size())
{
    // gathers information about the graph - input edges of every node and number of "consumers"
    // of all tensors in the graph
    for (int i = 0; i < graph.node_size(); ++i)
    {
        for (const auto& node_input : graph.node(i).input())
        {
            m_node_inputs[i].push_back(node_input);
            m_tensor_consumers[node_input] += 1;
        }
    }
}

void SubgraphExtractor::add_new_inputs(const std::vector<InputEdge>& new_inputs)
{
    for (const auto& input_edge : new_inputs)
    {
        validate_node_index(m_onnx_graph, input_edge.m_node_idx);

        // if a tensor has multiple consumers, its producer(source) should be replaced with a new
        // input - this way all consumers of this tensor will now be connected to a new graph input
        const auto tensor_name = get_input_tensor_name(m_onnx_graph, input_edge);
        if (m_tensor_consumers[tensor_name] > 1)
        {
            // remove a node or initializer from a model and insert a new input instead
            int idx = replace_source_with_new_input(m_onnx_graph, input_edge);
            if (idx != -1)
            {
                // if a node was replaced with an input, remove input edges from a helper multimap
                // for this node because it won't end up in the target subgraph
                // m_node_inputs stores information about existing edges in the graph,
                // when a node is removed/replaced, information about its edges should also
                // be removed (this way this node will be discarded from the original graph)
                m_node_inputs.erase(std::begin(m_node_inputs) + idx);
            }
        }
        else
        {
            // in case an edge is connected to a single node, a new graph input should be added
            // and connected to that node; the new edge is an edge between the node and new input
            const auto new_input = append_new_graph_input(m_onnx_graph, input_edge);
            if (new_input.first)
            {
                // the original edge should be replaced with a new one in the helper multimap
                // this information will later be used during the subgraph extraction stage
                replace_input_edge(input_edge, new_input.second);
            }
        }
    }
}

void SubgraphExtractor::add_new_outputs(const std::vector<OutputEdge>& new_outputs)
{
    for (const auto& output_edge : new_outputs)
    {
        validate_node_index(m_onnx_graph, output_edge.m_node_idx);

        append_new_graph_output(m_onnx_graph, output_edge);
    }
}

void SubgraphExtractor::replace_input_edge(const InputEdge& old_edge,
                                           const std::string& new_edge_name)
{
    // remove the old edge from the helper map and insert a new edge
    const auto pos_to_replace_it =
        std::begin(m_node_inputs.at(old_edge.m_node_idx)) + old_edge.m_port_idx;
    m_node_inputs.at(old_edge.m_node_idx).erase(pos_to_replace_it);
    m_node_inputs.at(old_edge.m_node_idx).insert(pos_to_replace_it, new_edge_name);
}

void SubgraphExtractor::extract_subgraph(std::vector<OutputEdge> subgraph_outputs)
{
    // when the user doesn't specify any outputs, all outputs of the original graph should be kept
    if (subgraph_outputs.empty())
    {
        subgraph_outputs = all_output_edges();
    }

    SubgraphComponents subgraph;

    for (const auto& output_edge : subgraph_outputs)
    {
        // for each output edge find the nodes, inputs and initializers that contribute to the value
        // produced by this output - "output contributors"
        // a sum of all contributors of all outputs is the target subgraph
        subgraph += discover_output_contributors(output_edge, subgraph);
    }

    // using the subgraph components collected above, modify the underlying GraphProto
    extract_subgraph_from_onnx_model(subgraph);
}

SubgraphExtractor::SubgraphComponents SubgraphExtractor::discover_output_contributors(
    const OutputEdge& output_edge, const SubgraphComponents& already_collected) const
{
    const auto already_visited = [&already_collected](const int node_index) {
        return already_collected.nodes.count(node_index) > 0;
    };

    SubgraphComponents output_contributors;
    const auto tensor_name = get_output_tensor_name(m_onnx_graph, output_edge);
    output_contributors.outputs.insert(tensor_name);

    // reverse DFS graph traversal
    std::stack<int> nodes_to_visit;
    nodes_to_visit.push(output_edge.m_node_idx);

    while (!nodes_to_visit.empty())
    {
        const auto n = nodes_to_visit.top();
        nodes_to_visit.pop();

        // if a node has already been visited, return early because it's already marked as
        // a node to keep in the final extracted subgraph
        if (already_visited(n))
        {
            continue;
        }

        output_contributors.nodes.insert(n);

        // check if the visitor reached any of the graph inputs
        // and/or keep looking for more contributors further up in the graph

        // when an input or initializer is reached, the visitor stops the lookup
        const auto n_inputs = m_node_inputs[n];
        for (auto& input_name : n_inputs)
        {
            if (is_graph_input(m_onnx_graph, input_name))
            {
                output_contributors.inputs.insert(input_name);
                // when an initializer has a matching graph input
                if (is_graph_initializer(m_onnx_graph, input_name))
                {
                    output_contributors.initializers.insert(input_name);
                }
            }
            else if (is_graph_initializer(m_onnx_graph, input_name))
            {
                // when an initializer doesn't have a corresponding input
                output_contributors.initializers.insert(input_name);
            }
            else
            {
                // if an edge points to another node (source node) it should be visited
                // in one of the future iterations
                nodes_to_visit.push(find_source_node_idx(m_onnx_graph, n, input_name));
            }
        }
    }

    return output_contributors;
}

void SubgraphExtractor::extract_subgraph_from_onnx_model(const SubgraphComponents& subgraph)
{
    discard_by_name(*(m_onnx_graph.mutable_input()), subgraph.inputs);
    discard_by_name(*(m_onnx_graph.mutable_initializer()), subgraph.initializers);
    discard_by_name(*(m_onnx_graph.mutable_output()), subgraph.outputs);
    discard_nodes(*(m_onnx_graph.mutable_node()), subgraph.nodes);
}

std::vector<OutputEdge> SubgraphExtractor::all_output_edges() const
{
    std::vector<OutputEdge> all_outputs;

    int output_port = 0;
    for (const auto& graph_output : m_onnx_graph.output())
    {
        const auto node_index =
            find_source_node_idx(m_onnx_graph, m_onnx_graph.node_size(), graph_output.name());
        all_outputs.emplace_back(node_index, output_port++);
    }

    return all_outputs;
}
