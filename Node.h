#ifndef BasicNode
#define BasicNode

/*
*  Node.h:
*  basic processing unit in a neural network
*  (1) we have a node structure to build user graph
*  (2) we have a execute structure to merge similar nodes that can be execute together
*  The real forward and backward are defined in Execute.
*  Every operation should define a node class and a execute class together.
*
*  Created on: Apr 21, 2017
*      Author: mszhang
*/
#include <iomanip>
#include "MyTensor.h"
#if USE_GPU
#include "n3ldg_cuda.h"
using n3ldg_cuda::Tensor1D;
using n3ldg_cuda::Tensor2D;
#else
using n3ldg_cpu::Tensor1D;
using n3ldg_cpu::Tensor2D;
#endif

class Execute;

#if USE_GPU
struct NodeInfo {
    dtype *val;
    dtype *loss;
    std::vector<dtype *> input_vals;
    std::vector<dtype *> input_losses;
    std::vector<int32_t> input_dims;

    NodeInfo() = default;
    NodeInfo(const NodeInfo &) = default;
    NodeInfo(NodeInfo &&) = default;
};

int GraphToMemory(const std::vector<std::vector<NodeInfo>> &graph,
        void *memory, std::vector<int> &offsets, int size) {
    assert(offsets.empty());
    int offset = 0;
    char *m = (char*)memory;
    for (const std::vector<NodeInfo> &vec : graph) {
        offsets.push_back(offset);
        for (const NodeInfo &node_info : vec) {
            *(dtype**)(m + offset) = node_info.val;
            offset += sizeof(node_info.val);
        }
        for (const NodeInfo &node_info : vec) {
            *(dtype**)(m + offset) = node_info.loss;
            offset += sizeof(node_info.loss);
        }
        for (const NodeInfo &node_info : vec) {
            if (!node_info.input_vals.empty()) {
                int len = node_info.input_vals.size() *
                        sizeof(node_info.input_vals.at(0));
                memcpy((void*)(m + offset), node_info.input_vals.data(), len);
                offset += len;
            }
        }
        for (const NodeInfo &node_info : vec) {
            if (!node_info.input_losses.empty()) {
                int len = node_info.input_losses.size() *
                        sizeof(node_info.input_losses.at(0));
                memcpy((void*)(m + offset),
                        node_info.input_losses.data(), len);
                offset += len;
            }
        }
        const NodeInfo &node_info = vec.at(0);
        if (!node_info.input_dims.empty()) {
            int len = node_info.input_dims.size() *
                    sizeof(node_info.input_dims.at(0));
            memcpy((void*)(m + offset), node_info.input_dims.data(), len);
            offset += len;
        }
    }
    if (offset > size) {
        std::cout << "actual_size is " << offset <<
            " but allocated size is " << size << std::endl;
        abort();
    }

//    std::cout << "graph size:" << offset << std::endl;
//    int i = 0;
//    for (int i = 0; i < offsets.size(); ++i) {
//        int offset = offsets.at(i);
//        std::cout << "offset:" << offset << std::endl;
//        char *m = (char*)memory + offset;
//        std::cout << "val:" << std::endl;
//        for (int j = 0; j < graph.at(i).size(); ++j) {
//            std::cout << "memory:" << *(dtype**)(m + j * sizeof(dtype*)) <<
//                " node:" << graph.at(i).at(j).val << std::endl;
//        }
//        m += graph.at(i).size() * sizeof(dtype*);
//        std::cout << "loss:" << std::endl;
//        for (int j = 0; j < graph.at(i).size(); ++j) {
//            std::cout << "memory:" << *(dtype**)(m + j * sizeof(dtype*)) <<
//                " node:" << graph.at(i).at(j).loss << std::endl;
//        }
//        m += graph.at(i).size() * sizeof(dtype*);

//        std::cout << "input val:" << std::endl;
//        int input_size = graph.at(i).at(0).input_vals.size();
//        for (int j = 0; j < graph.at(i).size(); ++j) {
//            for (int k = 0; k < input_size; ++k) {
//                std::cout << "memory:" <<
//                    *(dtype**)(m + (j * input_size + k) * sizeof(dtype*))
//                    << " node:" << graph.at(i).at(j).input_vals.at(k) <<
//                    std::endl;
//            }
//        }
//        m += input_size * graph.at(i).size() * sizeof(dtype*);

//        std::cout << "input loss:" << std::endl;
//        input_size = graph.at(i).at(0).input_losses.size();
//        for (int j = 0; j < graph.at(i).size(); ++j) {
//            for (int k = 0; k < input_size; ++k) {
//                std::cout << "memory:" <<
//                    *(dtype**)(m + (j * input_size + k) * sizeof(dtype*))
//                    << " node:" << graph.at(i).at(j).input_losses.at(k) <<
//                    std::endl;
//            }
//        }
//        m += input_size * graph.at(i).size() * sizeof(dtype*);

//        std::cout << "input dim:" << std::endl;
//        input_size = graph.at(i).at(0).input_dims.size();
//        for (int k = 0; k < input_size; ++k) {
//            std::cout << "memory:" <<
//                *(int*)(m + k * sizeof(int32_t))
//                << " node:" << graph.at(i).at(0).input_dims.at(k) <<
//                std::endl;
//        }
//    }

    return offset;
}

#endif

// one Node means a vector
// the col should be 1, because we aimed for NLP only
class Node {
  public:
    vector<Node*> parents;
  public:
    Tensor1D val;
    Tensor1D loss;
  public:
    int dim;
    int degree;
    string node_type;

  public:
    Tensor1D drop_mask;
    dtype drop_value;

  public:
    Node() {
        dim = 0;
        degree = 0;
        parents.clear();
        node_type = "interface";
        drop_value = -1;
    }

    virtual ~Node() = default;

  public:
    virtual inline void clearValue() {
#if !USE_GPU || TEST_CUDA
        val = 0;
        loss = 0;
#endif
        degree = 0;
        if (drop_value > 0)drop_mask = 1;
        parents.clear();
    }

    virtual inline void init(int ndim, dtype dropout) {
        dim = ndim;
#if TEST_CUDA || !USE_GPU
        val.init(dim);
        loss.init(dim);
#else
        val.initOnDevice(dim);
        loss.initOnDevice(dim);
#endif
#if USE_GPU
        n3ldg_cuda::Memset(val.value, dim, 0.0f);
        n3ldg_cuda::Memset(loss.value, dim, 0.0f);
#endif
        drop_mask.init(dim);
        if (dropout > 0 && dropout <= 1) {
            drop_value = dropout;
        } else {
            drop_value = -1;
        }
        parents.clear();
    }

    virtual void generate_dropmask(dtype drop_factor) {
        int dropNum = (int)(dim * drop_value * drop_factor);
        vector<int> tmp_masks(dim);
        for (int idx = 0; idx < dim; idx++) {
            tmp_masks[idx] = idx < dropNum ? 0 : 1;
        }
        random_shuffle(tmp_masks.begin(), tmp_masks.end());
        for (int idx = 0; idx < dim; idx++) {
            drop_mask[idx] = 1.0 * tmp_masks[idx];
        }
    }

    void forward_drop(bool bTrain, dtype drop_factor) {
        if (drop_value > 0) {
            if (bTrain) {
#if !TEST_CUDA
                generate_dropmask(drop_factor);
#endif
                val.vec() = val.vec() * drop_mask.vec();
            } else {
                val.vec() = val.vec() * (1 - drop_value * drop_factor);
            }
        }
        degree = -1;
    }

    inline void backward_drop() {
        if (drop_value > 0) {
            loss.vec() = loss.vec() * drop_mask.vec();
        }
    }

  public:
    virtual inline void compute() = 0;
    virtual inline void backward() = 0;

    virtual inline Execute* generate(bool bTrain, dtype cur_drop_factor) = 0;

    virtual bool typeEqual(Node* other) {
        if (node_type.compare(other->node_type) != 0) {
            return false;
        }
#if USE_GPU
        if (dim != other->dim) {
            return false;
        }
        if (!isEqual(drop_value, other->drop_value)) {
            return false;
        }
#endif
        return true;
    }

  public:
    virtual inline void addParent(Node* parent) {
        if (degree >= 0) {
            parents.push_back(parent);
            parent->degree++;
        }
    }

#if USE_GPU
    virtual void toNodeInfo(NodeInfo &node_info) const {
        node_info.val = val.value;
        node_info.loss = loss.value;
    }
#endif
};

typedef  Node* PNode;


#if USE_GPU
void clearNodes(std::vector<Node*> &nodes, int dim) {
    std::vector<dtype*> val_and_losses;
    val_and_losses.reserve(2 * nodes.size());
    for (Node *n : nodes) {
        val_and_losses.push_back(n->val.value);
        val_and_losses.push_back(n->loss.value);
    }
    n3ldg_cuda::BatchMemset(val_and_losses, val_and_losses.size(), dim,
            0.0f);
}
#endif

class Execute {
public:
    vector<PNode> batch;
    dtype drop_factor;
#if USE_GPU
    void *graph_info;
#endif

    virtual ~Execute() = default;

  public:
    virtual void forward() = 0;
    virtual void backward() = 0;
    virtual void clearValue() {
        for (PNode p : batch) {
            p->clearValue();
        }
#if USE_GPU
        clearNodes(batch, batch.at(0)->dim);
#endif
    }

    virtual bool addNode(PNode in) {
        if (batch.empty()) {
            std::cout << "empty batch, strange...." << std::endl;
            return false;
        }

        if (batch[0]->typeEqual(in)) {
            batch.push_back(in);
            return true;
        }

        return false;
    }
};


typedef  Execute* PExecute;


#endif
