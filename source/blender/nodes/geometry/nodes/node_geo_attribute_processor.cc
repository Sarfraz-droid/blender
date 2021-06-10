/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "BLI_kdopbvh.h"
#include "BLI_kdtree.h"
#include "BLI_task.hh"
#include "BLI_timeit.hh"

#include "DNA_mesh_types.h"

#include "BKE_bvhutils.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "NOD_node_tree_multi_function.hh"
#include "NOD_type_callbacks.hh"

#include "FN_multi_function_network_evaluation.hh"

#include "node_geometry_util.hh"

static void geo_node_attribute_processor_layout(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);

  bNode *node = (bNode *)ptr->data;
  NodeGeometryAttributeProcessor *storage = (NodeGeometryAttributeProcessor *)node->storage;

  uiLayout *row = uiLayoutRow(layout, true);
  uiTemplateIDBrowse(
      row, C, ptr, "node_tree", nullptr, nullptr, nullptr, UI_TEMPLATE_ID_FILTER_ALL, nullptr);
  uiItemStringO(row, "", ICON_PLUS, "node.new_attribute_processor_group", "node_name", node->name);

  uiItemR(layout, ptr, "domain", 0, "Domain", ICON_NONE);

  bNodeTree *group = (bNodeTree *)node->id;
  if (group == nullptr) {
    return;
  }

  {
    uiLayout *col = uiLayoutColumn(layout, false);

    bNodeSocket *interface_socket = (bNodeSocket *)group->inputs.first;
    AttributeProcessorInputSettings *input_settings = (AttributeProcessorInputSettings *)
                                                          storage->inputs_settings.first;
    for (; interface_socket && input_settings;
         interface_socket = interface_socket->next, input_settings = input_settings->next) {
      PointerRNA input_ptr;
      RNA_pointer_create(
          ptr->owner_id, &RNA_AttributeProcessorInputSettings, input_settings, &input_ptr);
      uiItemR(col, &input_ptr, "input_mode", 0, interface_socket->name, ICON_NONE);
    }
  }
  {
    uiLayout *col = uiLayoutColumn(layout, false);

    bNodeSocket *interface_socket = (bNodeSocket *)group->inputs.first;
    AttributeProcessorOutputSettings *output_settings = (AttributeProcessorOutputSettings *)
                                                            storage->outputs_settings.first;
    for (; interface_socket && output_settings;
         interface_socket = interface_socket->next, output_settings = output_settings->next) {
      PointerRNA input_ptr;
      RNA_pointer_create(
          ptr->owner_id, &RNA_AttributeProcessorOutputSettings, output_settings, &input_ptr);
      uiItemR(col, &input_ptr, "output_mode", 0, interface_socket->name, ICON_NONE);
    }
  }
}

static void geo_node_attribute_processor_init(bNodeTree *ntree, bNode *node)
{
  NodeGeometryAttributeProcessor *node_storage = (NodeGeometryAttributeProcessor *)MEM_callocN(
      sizeof(NodeGeometryAttributeProcessor), __func__);

  node->storage = node_storage;

  nodeAddSocket(ntree, node, SOCK_IN, "NodeSocketGeometry", "Geometry", "Geometry");
  nodeAddSocket(ntree, node, SOCK_IN, "NodeSocketString", "Selection", "Selection");
  nodeAddSocket(ntree, node, SOCK_OUT, "NodeSocketGeometry", "Geometry", "Geometry");
}

namespace blender::nodes {

static void free_input_settings(AttributeProcessorInputSettings *input_settings)
{
  MEM_freeN(input_settings->identifier);
  MEM_freeN(input_settings);
}

static void free_output_settings(AttributeProcessorOutputSettings *output_settings)
{
  MEM_freeN(output_settings->identifier);
  MEM_freeN(output_settings);
}

static void free_inputs_settings(ListBase *inputs_settings)
{
  LISTBASE_FOREACH_MUTABLE (AttributeProcessorInputSettings *, input_settings, inputs_settings) {
    free_input_settings(input_settings);
  }
  BLI_listbase_clear(inputs_settings);
}

static void free_outputs_settings(ListBase *outputs_settings)
{
  LISTBASE_FOREACH_MUTABLE (
      AttributeProcessorOutputSettings *, output_settings, outputs_settings) {
    free_output_settings(output_settings);
  }
  BLI_listbase_clear(outputs_settings);
}

static void geo_node_attribute_processor_storage_free(bNode *node)
{
  NodeGeometryAttributeProcessor *storage = (NodeGeometryAttributeProcessor *)node->storage;
  free_inputs_settings(&storage->inputs_settings);
  free_outputs_settings(&storage->outputs_settings);
  MEM_freeN(storage);
}

static void geo_node_attribute_processor_storage_copy(bNodeTree *UNUSED(dest_ntree),
                                                      bNode *dst_node,
                                                      const bNode *src_node)
{
  const NodeGeometryAttributeProcessor *src_storage = (const NodeGeometryAttributeProcessor *)
                                                          src_node->storage;
  NodeGeometryAttributeProcessor *dst_storage = (NodeGeometryAttributeProcessor *)MEM_callocN(
      sizeof(NodeGeometryAttributeProcessor), __func__);

  *dst_storage = *src_storage;

  BLI_listbase_clear(&dst_storage->inputs_settings);
  LISTBASE_FOREACH (
      const AttributeProcessorInputSettings *, src_input_settings, &src_storage->inputs_settings) {
    AttributeProcessorInputSettings *dst_input_settings = (AttributeProcessorInputSettings *)
        MEM_callocN(sizeof(AttributeProcessorInputSettings), __func__);
    *dst_input_settings = *src_input_settings;
    dst_input_settings->identifier = BLI_strdup(src_input_settings->identifier);
    BLI_addtail(&dst_storage->inputs_settings, dst_input_settings);
  }

  BLI_listbase_clear(&dst_storage->outputs_settings);
  LISTBASE_FOREACH (const AttributeProcessorOutputSettings *,
                    src_output_settings,
                    &src_storage->outputs_settings) {
    AttributeProcessorOutputSettings *dst_output_settings = (AttributeProcessorOutputSettings *)
        MEM_callocN(sizeof(AttributeProcessorOutputSettings), __func__);
    *dst_output_settings = *src_output_settings;
    dst_output_settings->identifier = BLI_strdup(src_output_settings->identifier);
    BLI_addtail(&dst_storage->outputs_settings, dst_output_settings);
  }

  dst_node->storage = dst_storage;
}

static void geo_node_attribute_processor_group_update(bNodeTree *ntree, bNode *node)
{
  NodeGeometryAttributeProcessor *storage = (NodeGeometryAttributeProcessor *)node->storage;
  bNodeTree *ngroup = (bNodeTree *)node->id;

  if (ngroup && (ID_IS_LINKED(ngroup) && (ngroup->id.tag & LIB_TAG_MISSING))) {
    /* Missing datablock, leave sockets unchanged so that when it comes back
     * the links remain valid. */
    return;
  }

  ListBase ngroup_inputs;
  ListBase ngroup_outputs;
  if (ngroup != nullptr) {
    ngroup_inputs = ngroup->inputs;
    ngroup_outputs = ngroup->outputs;
  }
  else {
    BLI_listbase_clear(&ngroup_inputs);
    BLI_listbase_clear(&ngroup_outputs);
  }

  Map<StringRefNull, bNodeSocket *> old_inputs_by_identifier;
  LISTBASE_FOREACH (bNodeSocket *, socket, &node->inputs) {
    old_inputs_by_identifier.add_new(socket->identifier, socket);
  }
  Map<StringRefNull, AttributeProcessorInputSettings *> old_inputs_settings_by_identifier;
  LISTBASE_FOREACH (AttributeProcessorInputSettings *, input_settings, &storage->inputs_settings) {
    old_inputs_settings_by_identifier.add_new(input_settings->identifier, input_settings);
  }
  Map<StringRefNull, AttributeProcessorOutputSettings *> old_outputs_settings_by_identifier;
  LISTBASE_FOREACH (
      AttributeProcessorOutputSettings *, output_settings, &storage->outputs_settings) {
    old_outputs_settings_by_identifier.add_new(output_settings->identifier, output_settings);
  }

  VectorSet<bNodeSocket *> new_inputs;
  VectorSet<AttributeProcessorInputSettings *> new_inputs_settings;
  VectorSet<AttributeProcessorOutputSettings *> new_output_settings;

  /* Keep geometry and selection socket. */
  new_inputs.add_new((bNodeSocket *)node->inputs.first);
  new_inputs.add_new(((bNodeSocket *)node->inputs.first)->next);

  LISTBASE_FOREACH (bNodeSocket *, interface_sock, &ngroup_inputs) {
    AttributeProcessorInputSettings *input_settings =
        old_inputs_settings_by_identifier.lookup_default(interface_sock->identifier, nullptr);

    char identifier1[MAX_NAME];
    char identifier2[MAX_NAME];
    BLI_snprintf(identifier1, sizeof(identifier1), "inA%s", interface_sock->identifier);
    BLI_snprintf(identifier2, sizeof(identifier2), "inB%s", interface_sock->identifier);

    if (input_settings == nullptr) {
      input_settings = (AttributeProcessorInputSettings *)MEM_callocN(
          sizeof(AttributeProcessorInputSettings), __func__);
      input_settings->identifier = BLI_strdup(interface_sock->identifier);
      if (!StringRef(interface_sock->default_attribute_name).is_empty()) {
        input_settings->input_mode = GEO_NODE_ATTRIBUTE_PROCESSOR_INPUT_MODE_DEFAULT;
      }
      BLI_addtail(&storage->inputs_settings, input_settings);

      new_inputs_settings.add_new(input_settings);
      bNodeSocket *sock_value = nodeAddSocket(
          ntree, node, SOCK_IN, interface_sock->idname, identifier1, interface_sock->name);
      bNodeSocket *sock_attribute = nodeAddSocket(
          ntree, node, SOCK_IN, "NodeSocketString", identifier2, interface_sock->name);
      node_socket_copy_default_value(sock_value, interface_sock);
      new_inputs.add_new(sock_value);
      new_inputs.add_new(sock_attribute);
    }
    else {
      new_inputs_settings.add_new(input_settings);
      bNodeSocket *sock_value = old_inputs_by_identifier.lookup(identifier1);
      bNodeSocket *sock_attribute = old_inputs_by_identifier.lookup(identifier2);
      STRNCPY(sock_value->name, interface_sock->name);
      STRNCPY(sock_attribute->name, interface_sock->name);
      new_inputs.add_new(sock_value);
      new_inputs.add_new(sock_attribute);
    }
  }
  LISTBASE_FOREACH (bNodeSocket *, interface_sock, &ngroup_outputs) {
    AttributeProcessorOutputSettings *output_settings =
        old_outputs_settings_by_identifier.lookup_default(interface_sock->identifier, nullptr);

    char identifier[MAX_NAME];
    BLI_snprintf(identifier, sizeof(identifier), "out%s", interface_sock->identifier);

    if (output_settings == nullptr) {
      output_settings = (AttributeProcessorOutputSettings *)MEM_callocN(
          sizeof(AttributeProcessorOutputSettings), __func__);
      output_settings->identifier = BLI_strdup(interface_sock->identifier);
      if (interface_sock->default_attribute_name[0] != '\0') {
        output_settings->output_mode = GEO_NODE_ATTRIBUTE_PROCESSOR_OUTPUT_MODE_DEFAULT;
      }
      BLI_addtail(&storage->outputs_settings, output_settings);

      new_output_settings.add_new(output_settings);
      new_inputs.add_new(nodeAddSocket(
          ntree, node, SOCK_IN, "NodeSocketString", identifier, interface_sock->name));
    }
    else {
      new_output_settings.add_new(output_settings);
      bNodeSocket *socket = old_inputs_by_identifier.lookup(identifier);
      STRNCPY(socket->name, interface_sock->name);
      new_inputs.add_new(socket);
    }
  }

  /* Clear the maps to avoid accidental access later on when they point to invalid memory. */
  old_inputs_by_identifier.clear();
  old_inputs_settings_by_identifier.clear();
  old_outputs_settings_by_identifier.clear();

  /* Remove unused data. */
  LISTBASE_FOREACH_MUTABLE (bNodeSocket *, socket, &node->inputs) {
    if (!new_inputs.contains(socket)) {
      nodeRemoveSocket(ntree, node, socket);
    }
  }
  LISTBASE_FOREACH_MUTABLE (
      AttributeProcessorInputSettings *, input_settings, &storage->inputs_settings) {
    if (!new_inputs_settings.contains(input_settings)) {
      BLI_remlink(&storage->inputs_settings, input_settings);
      free_input_settings(input_settings);
    }
  }
  LISTBASE_FOREACH_MUTABLE (
      AttributeProcessorOutputSettings *, output_settings, &storage->outputs_settings) {
    if (!new_output_settings.contains(output_settings)) {
      BLI_remlink(&storage->outputs_settings, output_settings);
      free_output_settings(output_settings);
    }
  }

  /* Sort remaining sockets and settings. */
  BLI_listbase_clear(&node->inputs);
  for (bNodeSocket *socket : new_inputs) {
    BLI_addtail(&node->inputs, socket);
  }
  BLI_listbase_clear(&storage->inputs_settings);
  for (AttributeProcessorInputSettings *input_settings : new_inputs_settings) {
    BLI_addtail(&storage->inputs_settings, input_settings);
  }
  BLI_listbase_clear(&storage->outputs_settings);
  for (AttributeProcessorOutputSettings *output_settings : new_output_settings) {
    BLI_addtail(&storage->outputs_settings, output_settings);
  }
}

static void geo_node_attribute_processor_update(bNodeTree *UNUSED(ntree), bNode *node)
{
  NodeGeometryAttributeProcessor *storage = (NodeGeometryAttributeProcessor *)node->storage;
  bNodeTree *group = (bNodeTree *)node->id;
  if (group == nullptr) {
    return;
  }

  bNodeSocket *next_socket = (bNodeSocket *)node->inputs.first;
  /* Skip geometry and selection socket. */
  next_socket = next_socket->next->next;
  LISTBASE_FOREACH (AttributeProcessorInputSettings *, input_settings, &storage->inputs_settings) {
    bNodeSocket *value_socket = next_socket;
    bNodeSocket *attribute_socket = value_socket->next;
    nodeSetSocketAvailability(
        value_socket, input_settings->input_mode == GEO_NODE_ATTRIBUTE_PROCESSOR_INPUT_MODE_VALUE);
    nodeSetSocketAvailability(attribute_socket,
                              input_settings->input_mode ==
                                  GEO_NODE_ATTRIBUTE_PROCESSOR_INPUT_MODE_ATTRIBUTE);
    next_socket = attribute_socket->next;
  }
  LISTBASE_FOREACH (
      AttributeProcessorOutputSettings *, output_settings, &storage->outputs_settings) {
    nodeSetSocketAvailability(next_socket,
                              output_settings->output_mode ==
                                  GEO_NODE_ATTRIBUTE_PROCESSOR_OUTPUT_MODE_ATTRIBUTE);
    next_socket = next_socket->next;
  }
}

static CustomDataType get_custom_data_type(const eNodeSocketDatatype type)
{
  switch (type) {
    case SOCK_FLOAT:
      return CD_PROP_FLOAT;
    case SOCK_VECTOR:
      return CD_PROP_FLOAT3;
    case SOCK_RGBA:
      return CD_PROP_COLOR;
    case SOCK_BOOLEAN:
      return CD_PROP_BOOL;
    case SOCK_INT:
      return CD_PROP_INT32;
    default:
      break;
  }
  BLI_assert_unreachable();
  return CD_PROP_FLOAT;
}

namespace {
struct InputsCache {
  Map<int, GVArrayPtr> group_inputs;
  Map<std::pair<std::string, CustomDataType>, GVArrayPtr> attributes;
  GVArrayPtr index;
};
}  // namespace

static IndexMask prepare_index_mask_from_selection(Vector<int64_t> &selection_indices,
                                                   const AttributeDomain domain,
                                                   const int domain_size,
                                                   const GeometryComponent &component,
                                                   const GeoNodeExecParams &geo_params)
{
  IndexMask selection;
  const std::string selection_name = geo_params.get_input<std::string>("Selection");
  if (selection_name.empty()) {
    selection = IndexRange(domain_size);
  }
  else {
    GVArray_Typed<bool> selection_attribute = component.attribute_get_for_read<bool>(
        selection_name, domain, false);
    for (const int i : IndexRange(domain_size)) {
      if (selection_attribute[i]) {
        selection_indices.append(i);
      }
    }
    selection = selection_indices.as_span();
  }
  return selection;
}

static bool load_input_varrays(InputsCache &inputs_cache,
                               const Span<DOutputSocket> used_group_inputs,
                               const NodeGeometryAttributeProcessor &storage,
                               const bNodeTree &group,
                               const GeoNodeExecParams &geo_params,
                               const GeometryComponent &component,
                               const AttributeDomain domain,
                               const int domain_size,
                               fn::MFParamsBuilder &fn_params)
{
  for (const DOutputSocket &dsocket : used_group_inputs) {
    const DNode dnode = dsocket.node();
    const GVArray *input_varray = nullptr;
    const bNodeSocketType *socket_typeinfo = dsocket->typeinfo();
    const CustomDataType data_type = get_custom_data_type(
        (eNodeSocketDatatype)socket_typeinfo->type);
    const CPPType *cpp_type = bke::custom_data_type_to_cpp_type(data_type);
    if (dnode->is_group_input_node()) {
      const int index = dsocket->index();
      input_varray = &*inputs_cache.group_inputs.lookup_or_add_cb(index, [&]() -> GVArrayPtr {
        const AttributeProcessorInputSettings *input_settings = (AttributeProcessorInputSettings *)
            BLI_findlink(&storage.inputs_settings, index);
        const bNodeSocket *interface_socket = (bNodeSocket *)BLI_findlink(&group.inputs, index);
        if (cpp_type == nullptr) {
          return {};
        }
        const StringRefNull identifier = interface_socket->identifier;
        GVArrayPtr input_varray;
        switch ((GeometryNodeAttributeProcessorInputMode)input_settings->input_mode) {
          case GEO_NODE_ATTRIBUTE_PROCESSOR_INPUT_MODE_ATTRIBUTE: {
            const std::string input_name = "inB" + identifier;
            const std::string attribute_name = geo_params.get_input<std::string>(input_name);
            return component.attribute_get_for_read(attribute_name, domain, data_type);
          }
          case GEO_NODE_ATTRIBUTE_PROCESSOR_INPUT_MODE_VALUE: {
            const std::string input_name = "inA" + identifier;
            GPointer value = geo_params.get_input(input_name);
            GVArrayPtr varray = std::make_unique<fn::GVArray_For_SingleValueRef>(
                *value.type(), domain_size, value.get());
            return varray;
          }
          case GEO_NODE_ATTRIBUTE_PROCESSOR_INPUT_MODE_DEFAULT: {
            const StringRef default_attribute_name = interface_socket->default_attribute_name;
            BUFFER_FOR_CPP_TYPE_VALUE(*cpp_type, buffer);
            socket_cpp_value_get(*interface_socket, buffer);
            GVArrayPtr varray = component.attribute_get_for_read(
                default_attribute_name, domain, data_type, buffer);
            cpp_type->destruct(buffer);
            return varray;
          }
        }
        BLI_assert_unreachable();
        return {};
      });
    }
    else if (dnode->idname() == "AttributeNodeIndex") {
      if (!inputs_cache.index) {
        auto get_index_func = [](const int64_t index) { return (int)index; };
        inputs_cache.index = std::make_unique<
            fn::GVArray_For_EmbeddedVArray<int, VArray_For_Func<int, decltype(get_index_func)>>>(
            domain_size, domain_size, get_index_func);
      }
      input_varray = &*inputs_cache.index;
    }
    else if (dnode->idname() == "ShaderNodeAttribute") {
      NodeShaderAttribute *storage = dnode->storage<NodeShaderAttribute>();
      const StringRefNull attribute_name = storage->name;

      input_varray = &*inputs_cache.attributes.lookup_or_add_cb(
          {attribute_name, data_type}, [&]() -> GVArrayPtr {
            return component.attribute_get_for_read(attribute_name, domain, data_type);
          });
    }
    else if (dnode->idname() == "AttributeNodeAttributeInput") {
      NodeAttributeAttributeInput *storage = dnode->storage<NodeAttributeAttributeInput>();
      const StringRefNull attribute_name = storage->attribute_name;
      input_varray = &*inputs_cache.attributes.lookup_or_add_cb(
          {attribute_name, data_type}, [&]() -> GVArrayPtr {
            return component.attribute_get_for_read(attribute_name, domain, data_type);
          });
    }
    else if (dnode->idname() == "AttributeNodePositionInput") {
      input_varray = &*inputs_cache.attributes.lookup_or_add_cb(
          {"position", CD_PROP_FLOAT3}, [&]() -> GVArrayPtr {
            return component.attribute_get_for_read("position", domain, CD_PROP_FLOAT3);
          });
    }

    if (input_varray == nullptr) {
      return false;
    }
    fn_params.add_readonly_single_input(*input_varray);
  }
  return true;
}

static bool prepare_group_outputs(const Span<DInputSocket> used_group_outputs,
                                  const NodeGeometryAttributeProcessor &storage,
                                  bNodeTree &group,
                                  GeoNodeExecParams &geo_params,
                                  GeometryComponent &component,
                                  const AttributeDomain domain,
                                  const int domain_size,
                                  fn::MFParamsBuilder &fn_params,
                                  Vector<std::unique_ptr<OutputAttribute>> &r_output_attributes)
{
  for (const DInputSocket &socket : used_group_outputs) {
    const DNode node = socket.node();
    std::string attribute_name;
    CustomDataType attribute_type;
    if (node->is_group_output_node()) {
      const int index = socket->index();
      const bNodeSocket *interface_socket = (bNodeSocket *)BLI_findlink(&group.outputs, index);
      const AttributeProcessorOutputSettings *output_settings =
          (AttributeProcessorOutputSettings *)BLI_findlink(&storage.outputs_settings, index);
      attribute_type = get_custom_data_type((eNodeSocketDatatype)interface_socket->typeinfo->type);
      switch ((GeometryNodeAttributeProcessorOutputMode)output_settings->output_mode) {
        case GEO_NODE_ATTRIBUTE_PROCESSOR_OUTPUT_MODE_ATTRIBUTE: {
          const StringRefNull identifier = interface_socket->identifier;
          const std::string socket_identifier = "out" + identifier;
          attribute_name = geo_params.extract_input<std::string>(socket_identifier);
          break;
        }
        case GEO_NODE_ATTRIBUTE_PROCESSOR_OUTPUT_MODE_DEFAULT: {
          attribute_name = StringRef(interface_socket->default_attribute_name);
          break;
        }
      }
    }
    else if (node->idname() == "AttributeNodeSetAttribute") {
      const NodeAttributeSetAttribute *storage = node->storage<NodeAttributeSetAttribute>();
      attribute_name = storage->attribute_name;
      attribute_type = get_custom_data_type((eNodeSocketDatatype)storage->type);
    }
    else if (node->idname() == "AttributeNodePositionOutput") {
      attribute_name = "position";
      attribute_type = CD_PROP_FLOAT3;
    }

    if (attribute_name.empty()) {
      return false;
    }

    auto attribute = std::make_unique<OutputAttribute>(
        component.attribute_try_get_for_output(attribute_name, domain, attribute_type));
    if (!*attribute) {
      /* Cannot create the output attribute. */
      return false;
    }
    GMutableSpan attribute_span = attribute->as_span();
    /* Destruct because the function expects an uninitialized array. */
    attribute_span.type().destruct_n(attribute_span.data(), domain_size);
    fn_params.add_uninitialized_single_output(attribute_span);
    r_output_attributes.append(std::move(attribute));
  }
  return true;
}

static void process_attributes_on_component(GeoNodeExecParams &geo_params,
                                            GeometryComponent &component,
                                            const fn::MultiFunction &network_fn,
                                            const Span<DOutputSocket> used_group_inputs,
                                            const Span<DInputSocket> used_group_outputs)
{
  const bNode &node = geo_params.node();
  bNodeTree *group = (bNodeTree *)node.id;
  const NodeGeometryAttributeProcessor &storage = *(NodeGeometryAttributeProcessor *)node.storage;
  const AttributeDomain domain = (AttributeDomain)storage.domain;

  const int domain_size = component.attribute_domain_size(domain);
  if (domain_size == 0) {
    return;
  }

  Vector<int64_t> selection_indices;
  const IndexMask selection = prepare_index_mask_from_selection(
      selection_indices, domain, domain_size, component, geo_params);

  fn::MFParamsBuilder fn_params{network_fn, domain_size};
  fn::MFContextBuilder context;

  InputsCache inputs_cache;
  if (!load_input_varrays(inputs_cache,
                          used_group_inputs,
                          storage,
                          *group,
                          geo_params,
                          component,
                          domain,
                          domain_size,
                          fn_params)) {
    return;
  }

  Vector<std::unique_ptr<OutputAttribute>> output_attributes;
  if (!prepare_group_outputs(used_group_outputs,
                             storage,
                             *group,
                             geo_params,
                             component,
                             domain,
                             domain_size,
                             fn_params,
                             output_attributes)) {
    return;
  }

  network_fn.call(selection, fn_params, context);

  for (std::unique_ptr<OutputAttribute> &output_attribute : output_attributes) {
    output_attribute->save();
  }
}

static void process_attributes(GeoNodeExecParams &geo_params, GeometrySet &geometry_set)
{
  const bNode &node = geo_params.node();
  bNodeTree *group = (bNodeTree *)node.id;

  if (group == nullptr) {
    return;
  }

  geometry_set = geometry_set_realize_instances(geometry_set);

  NodeTreeRefMap tree_refs;
  DerivedNodeTree tree{*group, tree_refs};
  fn::MFNetwork network;
  ResourceScope scope;
  MFNetworkTreeMap network_map = insert_node_tree_into_mf_network(network, tree, scope);

  Vector<const fn::MFOutputSocket *> fn_input_sockets;
  Vector<const fn::MFInputSocket *> fn_output_sockets;

  const DTreeContext &root_context = tree.root_context();
  const NodeTreeRef &root_tree_ref = root_context.tree();

  Span<const NodeRef *> output_nodes = root_tree_ref.nodes_by_type("NodeGroupOutput");

  if (output_nodes.size() >= 2) {
    return;
  }
  Vector<DInputSocket> used_group_outputs;
  if (output_nodes.size() == 1) {
    const DNode output_node{&root_context, output_nodes[0]};
    for (const InputSocketRef *socket_ref : output_node->inputs().drop_back(1)) {
      used_group_outputs.append({&root_context, socket_ref});
    }
  }
  tree.foreach_node_with_type("AttributeNodeSetAttribute", [&](const DNode dnode) {
    NodeAttributeSetAttribute *storage = dnode->storage<NodeAttributeSetAttribute>();
    if (storage->attribute_name[0] == '\0') {
      return;
    }
    for (const InputSocketRef *socket_ref : dnode->inputs()) {
      if (socket_ref->is_available()) {
        used_group_outputs.append({dnode.context(), socket_ref});
      }
    }
  });
  tree.foreach_node_with_type("AttributeNodePositionOutput", [&](const DNode dnode) {
    used_group_outputs.append(dnode.input(0));
  });
  if (used_group_outputs.is_empty()) {
    return;
  }

  Vector<fn::MFInputSocket *> network_outputs;
  for (const DInputSocket &socket : used_group_outputs) {
    network_outputs.append(network_map.lookup(socket).first());
  }

  VectorSet<const fn::MFOutputSocket *> network_inputs;
  VectorSet<const fn::MFInputSocket *> unlinked_inputs;
  network.find_dependencies(network_outputs, network_inputs, unlinked_inputs);
  BLI_assert(unlinked_inputs.is_empty());

  Vector<DOutputSocket> used_group_inputs;
  for (const fn::MFOutputSocket *dummy_socket : network_inputs) {
    const DOutputSocket dsocket = network_map.try_lookup(*dummy_socket);
    BLI_assert(dsocket);
    used_group_inputs.append(dsocket);
  }

  fn::MFNetworkEvaluator network_fn{Vector<const fn::MFOutputSocket *>(network_inputs.as_span()),
                                    Vector<const fn::MFInputSocket *>(network_outputs.as_span())};

  if (geometry_set.has_mesh()) {
    process_attributes_on_component(geo_params,
                                    geometry_set.get_component_for_write<MeshComponent>(),
                                    network_fn,
                                    used_group_inputs,
                                    used_group_outputs);
  }
  if (geometry_set.has_pointcloud()) {
    process_attributes_on_component(geo_params,
                                    geometry_set.get_component_for_write<PointCloudComponent>(),
                                    network_fn,
                                    used_group_inputs,
                                    used_group_outputs);
  }
  if (geometry_set.has_curve()) {
    process_attributes_on_component(geo_params,
                                    geometry_set.get_component_for_write<CurveComponent>(),
                                    network_fn,
                                    used_group_inputs,
                                    used_group_outputs);
  }
}

static void geo_node_attribute_processor_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  process_attributes(params, geometry_set);
  params.set_output("Geometry", geometry_set);
}

}  // namespace blender::nodes

void register_node_type_geo_attribute_processor()
{
  static bNodeType ntype;

  geo_node_type_base(
      &ntype, GEO_NODE_ATTRIBUTE_PROCESSOR, "Attribute Processor", NODE_CLASS_GROUP, 0);
  node_type_init(&ntype, geo_node_attribute_processor_init);
  node_type_storage(&ntype,
                    "NodeGeometryAttributeProcessor",
                    blender::nodes::geo_node_attribute_processor_storage_free,
                    blender::nodes::geo_node_attribute_processor_storage_copy);
  node_type_update(&ntype, blender::nodes::geo_node_attribute_processor_update);
  node_type_group_update(&ntype, blender::nodes::geo_node_attribute_processor_group_update);
  ntype.geometry_node_execute = blender::nodes::geo_node_attribute_processor_exec;
  ntype.draw_buttons = geo_node_attribute_processor_layout;
  nodeRegisterType(&ntype);
}