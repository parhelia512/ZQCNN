import sys,os
import onnx
import numpy as np
import struct
from onnx import shape_inference
from onnx import numpy_helper, ValueInfoProto, AttributeProto, GraphProto, NodeProto, TensorProto, TensorShapeProto
from typing import Any, Text, Iterable, List, Dict, Sequence, Optional, Tuple, Union
from typing_extensions import Protocol



os.environ['CUDA_VISIBLE_DEVICES'] = '-1'

EdgeInfo = Tuple[Text, Any, TensorShapeProto]
AttributeValue = Any # TODO Union[Sequence[float], Sequence[int], Sequence[Text], Sequence[TensorProto], Sequence[GraphProto]]

def _input_from_onnx_input(input):  # type: (ValueInfoProto) -> EdgeInfo
    name = input.name
    type = input.type.tensor_type.elem_type
    shape = tuple([d.dim_value for d in input.type.tensor_type.shape.dim])
    return (name, type, shape)

def _convertAttributeProto(onnx_arg):  # type: (AttributeProto) -> AttributeValue
    """
    Convert an ONNX AttributeProto into an appropriate Python object
    for the type.
    NB: Tensor attribute gets returned as numpy array
    """
    if onnx_arg.HasField('f'):
        return onnx_arg.f
    elif onnx_arg.HasField('i'):
        return onnx_arg.i
    elif onnx_arg.HasField('s'):
        return onnx_arg.s
    elif onnx_arg.HasField('t'):
        return numpy_helper.to_array(onnx_arg.t)
    elif len(onnx_arg.floats):
        return list(onnx_arg.floats)
    elif len(onnx_arg.ints):
        return list(onnx_arg.ints)
    elif len(onnx_arg.strings):
        return list(onnx_arg.strings)
    else:
        raise ValueError("Unsupported ONNX attribute: {}".format(onnx_arg))
        
class Attributes(Dict[Text, Any]):
    @staticmethod
    def from_onnx(args):  # type: (Iterable[AttributeProto]) -> Attributes
        d = Attributes()
        for arg in args:
            #print(arg.name)
            d[arg.name] = _convertAttributeProto(arg)
        return d

class Node(object):
    def __init__(self,
                 name,  # type: Optional[Text]
                 op_type,  # type: Text
                 attrs,  # type: Dict[Text, AttributeValue]
                 inputs,  # type: List[Text]
                 outputs,  # type: List[Text]
                 ):
        # type: (...) -> None
        self.name = name
        self.op_type = op_type
        self.attrs = attrs
        self.inputs = inputs
        self.outputs = outputs
        self.input_tensors = {}  # type: Dict[Text, np._ArrayLike[Any]]
        self.parents = []  # type: List[Node]
        self.children = []  # type: List[Node]
        self.metadata = {}  # type: Dict[Any, Any]

    def add_parent(self, parent_node):  # type: (Node) -> None
        assert parent_node not in self.parents
        self.parents.append(parent_node)
        if self not in parent_node.children:
            parent_node.children.append(self)

    def add_child(self, child_node):  # type: (Node) -> None
        assert child_node not in self.children
        self.children.append(child_node)
        if self not in child_node.parents:
            child_node.parents.append(self)

    def get_only_parent(self):  # type: () -> Node
        if len(self.parents) != 1:
            raise ValueError('Node ({}) expected to have 1 parent. Found {}.'
                             .format(self, len(self.parents)))
        return self.parents[0]

    @staticmethod
    def from_onnx(node):  # type: (NodeProto) -> Node
        #print(node.attribute)
        attrs = Attributes.from_onnx(node.attribute)
        #print(attrs)
        name = Text(node.name)
        if len(name) == 0:
            name = "_".join(node.output)
        return Node(
            name, node.op_type, attrs, list(node.input), list(node.output)
        )

class Graph(object):
    def __init__(self,
                 nodes,  # type: List[Node]
                 inputs,  # type: List[EdgeInfo]
                 outputs,  # type: List[EdgeInfo]
                 shape_dict, # type: Dict[Text,Tuple[int,...]]
                 ):
        # type: (...) -> None
        self.nodes = nodes
        self.inputs = inputs
        self.outputs = outputs
        self.shape_dict = shape_dict  # data blob name to its shape

        # data blob name to the list of op types it feeds into
        self.blob_to_op_type = {} # type: Dict[Text, List[Text]]
        # data blob name to the op_type that generates it
        self.blob_from_op_type = {}  # type: Dict[Text, Text]

        for node_ in nodes:
            for input_ in node_.inputs:
                if input_ in self.blob_to_op_type:
                    self.blob_to_op_type[input_].append(node_.op_type)
                else:
                    self.blob_to_op_type[input_] = [node_.op_type]
            for output_ in node_.outputs:
                if output_ in self.blob_from_op_type:
                    raise ValueError("Data blob: %s, is generated by more than 1 op" %(output_))
                self.blob_from_op_type[output_] = node_.op_type


    def transformed(self, transformers):  # type: (Iterable[Transformer]) -> Graph
        graph = self
        for transformer in transformers:
            graph = transformer(graph)
        return graph

    def has_edge_name(self, name):  # type: (Text) -> bool
        '''
        Check if name is already used for graph inputs/outputs or for nodes
        inputs/outputs
        '''
        names = set()
        for input in self.inputs:
            names.add(input[0])
        for output in self.outputs:
            names.add(output[0])
        for node in self.nodes:
            names.update(node.inputs)
            names.update(node.outputs)
        return name in names

    def get_unique_edge_name(self, name):  # type: (Text) -> Text
        n_ = name
        i = 0
        while self.has_edge_name(n_):
            n_ = "{}_{}".format(name, i)
            i += 1
        return n_

    @staticmethod
    def from_onnx(graph):  # type: (GraphProto) -> Graph
        input_tensors = {
            t.name: numpy_helper.to_array(t) for t in graph.initializer
        }
        nodes_ = []
        nodes_by_input = {}  # type: Dict[Text, List[Node]]
        nodes_by_output = {}
        for node in graph.node:
            node_ = Node.from_onnx(node)
            for input_ in node_.inputs:
                if input_ in input_tensors:
                    node_.input_tensors[input_] = input_tensors[input_]
                else:
                    if input_ in nodes_by_input:
                        input_nodes = nodes_by_input[input_]
                    else:
                        input_nodes = []
                        nodes_by_input[input_] = input_nodes
                    input_nodes.append(node_)
            for output_ in node_.outputs:
                nodes_by_output[output_] = node_
            nodes_.append(node_)

        inputs = []
        for i in graph.input:
            if i.name not in input_tensors:
                inputs.append(_input_from_onnx_input(i))

        outputs = []
        for o in graph.output:
            outputs.append(_input_from_onnx_input(o))

        for node_ in nodes_:
            for input_ in node_.inputs:
                if input_ in nodes_by_output:
                    node_.parents.append(nodes_by_output[input_])
            for output_ in node_.outputs:
                if output_ in nodes_by_input:
                    node_.children.extend(nodes_by_input[output_])

        # Dictionary to hold the "value_info" field from ONNX graph
        shape_dict = {} # type: Dict[Text,Tuple[int,...]]

        def extract_value_info(shape_dict, # type: Dict[Text,Tuple[int,...]]
                               value_info, # type: ValueInfoProto[...]
                               ):
            # type: (...) -> None
            shape_dict[value_info.name] = tuple([int(dim.dim_value) for dim in value_info.type.tensor_type.shape.dim])

        for value_info in graph.value_info:
            extract_value_info(shape_dict, value_info)
        for value_info in graph.input:
            extract_value_info(shape_dict, value_info)
        for value_info in graph.output:
            extract_value_info(shape_dict, value_info)


        return Graph(nodes_, inputs, outputs, shape_dict)

def getGraph(onnx_path):
    model = onnx.load(onnx_path)
    model = shape_inference.infer_shapes(model)
    model_graph = model.graph
    graph = Graph.from_onnx(model_graph)
    graph.channel_dims = {}

    return graph
    
def get_NCHW(tensor):
    tensor_shape = tensor.shape
    dim_num = len(tensor_shape)
    N,C,H,W = [1,1,1,1]
    if dim_num == 4:
        N = tensor_shape[0]
        C = tensor_shape[1]
        H = tensor_shape[2]
        W = tensor_shape[3]
    elif dim_num == 3:
        C = tensor_shape[0]
        H = tensor_shape[1]
        W = tensor_shape[2]
    elif dim_num == 2:
        N = tensor_shape[0]
        C = tensor_shape[1]
    elif dim_num == 1:
        C = tensor_shape[0]
    return [N,C,H,W]
	
    
def put_node_binaray_to_file(fout2, tensor, conv2d_transpose = False, need_add_eps = False, eps = 0.001):
    tensor_shape = tensor.shape
    dim_num = len(tensor_shape)
    N,C,H,W = [1,1,1,1]
    if dim_num == 4:
        N = tensor_shape[0]
        C = tensor_shape[1]
        H = tensor_shape[2]
        W = tensor_shape[3]
    elif dim_num == 3:
        C = tensor_shape[0]
        H = tensor_shape[1]
        W = tensor_shape[2]
    elif dim_num == 2:
        N = tensor_shape[0]
        C = tensor_shape[1]
    elif dim_num == 1:
        C = tensor_shape[0]
        
    tensor_content = tensor.data.tobytes()
    num_bytes = len(tensor_content)
    #print(num_bytes)
    if num_bytes == 0:
        #print('num_float=0')
        num_float = N*C*H*W
        float_val = tensor_tensor.float_val[0]
        #print(dir(float_val))
        s = struct.pack('f', float_val)
        for j in range(num_float):
            fout2.write(s)		
    else:
        if dim_num == 1:
            num_float = int(num_bytes/4)
            float_weights = struct.unpack('<%df'%num_float, struct.pack('%dB'%num_bytes, *tensor_content))
            if need_add_eps:
                float_weights0 = ()
                for k in range(num_float):
                    float_weights0 = float_weights0 + (float_weights[k]+eps,)
                float_weights = float_weights0
            fout2.write(struct.pack('%df'%num_float, *float_weights))
        else:
            num_float = int(num_bytes/4)
            #print('num_float=%d'%num_float)
            #print(type(tensor_content))
            #print(dir(tensor_content))
            float_weights = struct.unpack('<%df'%num_float, struct.pack('%dB'%num_bytes, *tensor_content))
            if need_add_eps:
                float_weights0 = ()
                for k in range(num_float):
                    float_weights0 = float_weights0 + (float_weights[k]+eps,)
                float_weights = float_weights0
            if conv2d_transpose:
                float_weights1 = _H_WNC_to_NCHW(float_weights,N,C,H,W)
            else:
                float_weights1 = float_weights
            #print(float_weights)
            #print(float_weights1)
            #fout2.write(struct.pack('%dB'%num_bytes, *tensor_content))
            fout2.write(struct.pack('%df'%num_float, *float_weights1))
       
def convertToZQCNN(graph, param_file, bin_file):
    fout = open(param_file,"w")
    fout2 = open(bin_file,"wb")
   
    exist_edges = []
    layers = []
    exist_nodes = []
    for input_node in graph.inputs:
        #print(input_node)
        name = input_node[0]
        output = input_node[0]
        shape = input_node[2]
        shape = list(shape)
        line = 'Input' + ' name=' + name
        size_num = len(shape)
        if size_num == 4:
            line = line + ' C=%d H=%d W=%d'%(shape[1],shape[2],shape[3])
        #print(line)
        fout.write(line+'\n');    
    

    
    for id, node in enumerate(graph.nodes):
        node_name = node.name
        op_type = node.op_type
        #print(op_type)
        #exit(0)
        line = ''
        if op_type == 'Conv':
            #print(node_name, node.inputs, node.outputs, node.attrs)
            #print(type(node.inputs))
            
            input_name = str(node.inputs[0])
            output_name = str(node.outputs[0])
            weight_name = node.inputs[1]
            if weight_name in node.input_tensors:
                weight = node.input_tensors[weight_name]
            else:
                print('failed to convert this node:', node_name, inputs, outputs)
                exit(0)
            has_bias = len(node.inputs) >= 3
            if has_bias:
                bias_name = node.inputs[2]
                if bias_name in node.input_tensors:
                    bias_weight = node.input_tensors[bias_name]
                else:
                    print('failed to convert this node:', node_name, inputs, outputs)
                    exit(0)
            
            [N,C,H,W] = get_NCHW(weight)
            dilations = node.attrs.get("dilations", [1, 1])
            # groups = 1
            groups = node.attrs.get("group", 1)
            #print(groups)
            kernel_shape = node.attrs["kernel_shape"]
            pads = node.attrs.get("pads", [0, 0, 0, 0])
            strides = node.attrs["strides"]
            if groups == 1:
                line = 'Convolution name=' + node_name.replace(':','_')
            else:
                line = 'DepthwiseConvolution name=' + node_name.replace(':','_')
            line = line + ' bottom=' + input_name.replace(':','_') + ' top=' + output_name.replace(':','_')
            line = line + ' num_output=%d kernel_H=%d kernel_W=%d dilate_H=%d dilate_W=%d stride_H=%d stride_W=%d'%(N,H,W,dilations[0],dilations[1],strides[0],strides[1])
            line = line + ' pad_H_top=%d pad_H_bottom=%d pad_W_left=%d pad_W_right=%d'%(pads[0],pads[1],pads[2],pads[3])
            put_node_binaray_to_file(fout2, weight, False, False, 0.001)
            if has_bias:
                put_node_binaray_to_file(fout2, bias_weight, False, False, 0.001)
                line = line + ' bias'
            #print(line)
        elif op_type == 'Relu':
            #print(node_name, node.inputs, node.outputs, node.attrs)
            input_name = str(node.inputs[0])
            output_name = str(node.outputs[0])
            line = 'ReLU name=' + node_name.replace(':','_') + ' bottom=' + input_name.replace(':','_') + ' top=' + output_name.replace(':','_')
            #print(line)
        elif op_type == 'Upsample':
            #print(node_name, node.inputs, node.outputs, node.attrs)
            #print(node.input_tensors)
            weight_name = node.inputs[1]
            if weight_name in node.input_tensors:
                weight = node.input_tensors[weight_name]
            else:
                print('failed to convert this node:', node_name, inputs, outputs)
                exit(0)
            mode = node.attrs['mode']
            if mode == 'linear':
                line = 'UpSampling sample_type=bilinear name=' + node_name.replace(':','_')
            else:
                line = 'UpSampling sample_type=nearest name=' + node_name.replace(':','_')
            input_name = node.inputs[0]
            output_name = node.outputs[0]
            line = line + ' bottom=' + input_name.replace(':','_') + ' top=' + output_name.replace(':','_')
            line = line + ' scale_h=%f scale_w=%f'%(weight[2],weight[3])
            #print(line)
        elif op_type == 'Concat':
            #print(node_name, node.inputs, node.outputs, node.attrs)
            axis = node.attrs['axis']
            line = 'Concat name=' + node_name
            in_num = len(node.inputs)
            for j in range(in_num):
                line = line + ' bottom=' + node.inputs[j].replace(':','_')
            line = line + ' top=' + node.outputs[0].replace(':','_') + ' axis=%d'%axis
            #print(line)
        elif op_type == 'Transpose':
            #print(node_name, node.inputs, node.outputs, node.attrs)
            order = node.attrs['perm']
            input_name = node.inputs[0]
            output_name = node.outputs[0]
            line = 'Permute name=' + node_name.replace(':','_') + ' bottom=' + input_name.replace(':','_') + ' top=' + output_name.replace(':','_')
            line = line + ' order=%d order=%d order=%d order=%d'%(order[0],order[1],order[2],order[3])
            #print(line)            
        elif op_type == 'Reshape':
            #print(node_name, node.inputs, node.outputs, node.attrs)
            weight_name = node.inputs[1]
            if weight_name in node.input_tensors:
                weight = node.input_tensors[weight_name]
            else:
                print('failed to convert this node:', node_name, inputs, outputs)
                exit(0)
            #print(weight)
            input_name = node.inputs[0]
            output_name = node.outputs[0]
            line = 'Reshape name=' + node_name.replace(':','_') + ' bottom=' + input_name.replace(':','_') + ' top=' + output_name.replace(':','_')
            for j in range(len(weight)):
                line = line + ' dim=%d'%(weight[j])
            for j in range(len(weight),4):
                line = line + ' dim=1'
            print(line)      
            
        else:
            print('unsupported layer: ', node_name, node.inputs, node.outputs, node.attrs)
        
        if line == '':
            pass
        else:
            fout.write(line + '\n')
            
    fout.close()
    fout2.close()
    print('done')
    
if __name__ == "__main__":
    os.environ
    graph = getGraph('model-face.onnx')
    convertToZQCNN(graph, 'model-face.zqparams','model-face.nchwbin')
    