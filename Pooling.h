#ifndef POOLING
#define POOLING

/*
*  Pooling.h:
*  pool operation, max, min, average and sum pooling
*
*  Created on: Apr 22, 2017
*      Author: mszhang
*/

#include "MyLib.h"
#include "Node.h"
#include "Graph.h"
#if USE_GPU
#include "n3ldg_cuda.h"
#endif
#include "profiler.h"

class PoolNode : public Node {
  public:
    vector<int> masks;
    vector<PNode> ins;

  public:
    PoolNode() : Node() {
        ins.clear();
        masks.clear();
    }

    ~PoolNode() {
        masks.clear();
        ins.clear();
    }

    inline void clearValue() {
        Node::clearValue();
        ins.clear();
        for(int idx = 0; idx < dim; idx++) {
            masks[idx] = -1;
        }
    }


    inline void init(int ndim, dtype dropout) {
        Node::init(ndim, dropout);
        masks.resize(ndim);
        for(int idx = 0; idx < ndim; idx++) {
            masks[idx] = -1;
        }
    }

  public:
    void forward(Graph *cg, const vector<PNode>& x) {
        if (x.size() == 0) {
            std::cout << "empty inputs for max|min|sum|avg pooling" << std::endl;
            return;
        }
        int nSize = x.size();
        ins.clear();
        for (int i = 0; i < nSize; i++) {
            if (x[i]->val.dim != dim) {
                std::cout << "input matrixes are not matched" << std::endl;
                clearValue();
                return;
            }
            ins.push_back(x[i]);
        }

        degree = 0;
        for (int i = 0; i < nSize; i++) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }


  public:
    inline PExecute generate(bool bTrain, dtype cur_drop_factor);

    // better to rewrite for deep understanding
    inline bool typeEqual(PNode other) {
        return Node::typeEqual(other);
    }

  public:
    virtual inline void setMask() = 0;

    inline void compute() {
        int nSize = ins.size();
        setMask();
        for(int i = 0; i < dim; i++) {
            int mask_i = masks.at(i);
            val[i] = ins.at(mask_i)->val[i];
        }
    }

    void backward() {
        for(int i = 0; i < dim; i++) {
            ins[masks[i]]->loss[i] += loss[i];
        }
    }

};

#if USE_GPU
class MaxPoolNode : public
#if TEST_CUDA
                    PoolNode
#else
                    Node
#endif
{
public:
#if !TEST_CUDA
    vector<PNode> ins;
#endif
    n3ldg_cuda::PageLockedNumberPointerArray dInValues;
    n3ldg_cuda::PageLockedNumberPointerArray dInLosses;

    MaxPoolNode() {
        node_type = "max-pooling";
    }

#if TEST_CUDA
    void setMask() {
        int nSize = ins.size();
        int thread_count = 8;
        while (thread_count < nSize) {
            thread_count <<= 1;
        }

        for (int dim_i = 0; dim_i < dim; ++dim_i) {
            dtype shared_arr[1024];
            dtype shared_indexers[1024];
            for (int i = 0; i < 1024; ++i) {
                shared_arr[i] = i < nSize ? ins[i]->val[dim_i] : -INFINITY;
                shared_indexers[i] = i;
            }
            for (int i = (thread_count >> 1); i > 0; i >>= 1) {
                for (int j = 0; j < i; ++j) {
                    int plus_i = j + i;
                    if (shared_arr[j] < shared_arr[plus_i]) {
                        shared_arr[j] = shared_arr[plus_i];
                        shared_indexers[j] = shared_indexers[plus_i];
                    }
                }

                masks[dim_i] = shared_indexers[0];
            }
        }
    }
#else
    void compute() {
        abort();
    }

    void backward() {
        abort();
    }
#endif

    void forward(Graph *cg, const vector<PNode>& x) {
        assert(!x.empty());
        int nSize = x.size();
        ins.clear();
        for (int i = 0; i < nSize; i++) {
            assert(x[i]->val.dim == dim);
            ins.push_back(x[i]);
        }

        degree = 0;
        for (int i = 0; i < nSize; i++) {
            ins[i]->addParent(this);
        }

        initDeviceMembers();
        cg->addNode(this);
    }

    void initDeviceMembers() {
        std::vector<dtype*> values;
        values.reserve(ins.size());
        std::vector<dtype*> losses;
        losses.reserve(ins.size());
        for (Node *n : ins) {
            values.push_back(n->val.value);
            losses.push_back(n->loss.value);
        }

        dInValues.init(values.data(), values.size());
        dInLosses.init(losses.data(), losses.size());
    }

    void toNodeInfo(NodeInfo &info) const override {
        Node::toNodeInfo(info);
        info.input_count = ins.size();
        for (PNode p : ins) {
            info.input_vals.push_back(p->val.value);
            info.input_losses.push_back(p->loss.value);
        }
    }

    PExecute generate(bool bTrain, dtype cur_drop_factor) override;
};
#else
class MaxPoolNode : public PoolNode {
  public:
    MaxPoolNode() : PoolNode() {
        node_type = "max-pooling";
    }

    void setMask() {
        int nSize = ins.size();

        for (int idx = 0; idx < dim; idx++) {
            int maxIndex = -1;
            for (int i = 0; i < nSize; ++i) {
                if (maxIndex == -1 || ins[i]->val[idx] > ins[maxIndex]->val[idx]) {
                    maxIndex = i;
                }
            }
            masks[idx] = maxIndex;
        }
    }

};
#endif



class MinPoolNode : public PoolNode {
  public:
    MinPoolNode() : PoolNode() {
        node_type = "min-pooling";
    }

  public:
    //Be careful that the row is the dim of input vector, and the col is the number of input vectors
    //Another point is that we change the input vectors directly.
    void setMask() {
        int nSize = ins.size();
        for (int idx = 0; idx < dim; idx++) {
            int minIndex = -1;
            for (int i = 0; i < nSize; ++i) {
                if (minIndex == -1 || ins[i]->val[idx] < ins[minIndex]->val[idx]) {
                    minIndex = i;
                }
            }
            masks[idx] = minIndex;
        }
    }

};

#if USE_GPU
class MaxPoolExecute : public Execute
{
public:
    int dim;
    n3ldg_cuda::IntArray hit_inputs;
    std::vector<int> in_counts;

    void forward() override {
        n3ldg_cuda::Profiler &profiler = n3ldg_cuda::Profiler::Ins();
        profiler.BeginEvent("MaxPoolNode forward");
        int count = batch.size();
        hit_inputs.init(count * dim);
        std::vector<dtype**> ins;
        ins.reserve(count);
        in_counts.reserve(count);
        std::vector<dtype*> outs;
        outs.reserve(count);
        for (Node *n : batch) {
            MaxPoolNode *m = static_cast<MaxPoolNode*>(n);
            ins.push_back(m->dInValues.value);
            in_counts.push_back(m->ins.size());
#if TEST_CUDA
            for (Node *nn : m->ins) {
                n3ldg_cuda::Assert(nn->val.verify(
                            "max pooling forward input"));
                nn->val.copyFromHostToDevice();
            }
#endif
            outs.push_back(n->val.value);
        }
        n3ldg_cuda::MaxPoolForward(graph_info, count, in_counts, dim,
                hit_inputs.value);
#if TEST_CUDA
        for (int idx = 0; idx < count; idx++) {
            batch[idx]->compute();
            n3ldg_cuda::Assert(batch[idx]->val.verify("max pooling forward"));
            MaxPoolNode *n = static_cast<MaxPoolNode*>(batch[idx]);
            if (!n3ldg_cuda::Verify(n->masks.data(),
                        hit_inputs.value + idx * dim, dim,
                        "max pooling forward mask")) {
                for (int i = 0; i < 2; ++i) {
                    std::cout << "i:" << i << std::endl;
                    Node *p = n->ins.at(i);
                    std::cout << n->ins.at(i)->val[37] << std::endl;
                }
                abort();
            }
        }
#endif
        profiler.EndCudaEvent();
    }

    void backward() override {
        int count = batch.size();
        std::vector<dtype*> losses;
        losses.reserve(count);
        std::vector<dtype**> in_losses;
        in_losses.reserve(count);
        for (Node *n : batch) {
            MaxPoolNode *m = static_cast<MaxPoolNode*>(n);
            n3ldg_cuda::Assert(m->loss.verify("max pooling backward loss"));
            losses.push_back(m->loss.value);
            in_losses.push_back(m->dInLosses.value);
#if TEST_CUDA
            int in_i = 0;
            for (Node *in : m->ins) {
                n3ldg_cuda::Assert(in->loss.verify(
                            "max pooling backward in loss initial"));
            }
#endif
        }
        n3ldg_cuda::MaxPoolBackward(graph_info, in_counts, hit_inputs.value,
                count, dim);

#if TEST_CUDA
        for (int idx = 0; idx < count; idx++) {
            batch[idx]->backward();
        }

        for (int idx = 0; idx < count; idx++) {
            int in_i = 0;
            for (Node *n : static_cast<MaxPoolNode*>(batch[idx])->ins) {
                n3ldg_cuda::Assert(n->loss.verify("max pooling backward"));
            }
        }
#endif
    }
};

PExecute MaxPoolNode::generate(bool bTrain, dtype cur_drop_factor) {
    MaxPoolExecute *exec = new MaxPoolExecute;
    exec->batch.push_back(this);
    exec->dim = dim;
    return exec;
}
#endif

class PoolExecute : public Execute {
  public:
    bool bTrain;
  public:
    virtual void  forward() {
        int count = batch.size();
        //#pragma omp parallel for
        for (int idx = 0; idx < count; idx++) {
            batch[idx]->compute();
            batch[idx]->forward_drop(bTrain, drop_factor);
        }
    }

    virtual void backward() {
        int count = batch.size();
        //#pragma omp parallel for
        for (int idx = 0; idx < count; idx++) {
            batch[idx]->backward_drop();
            batch[idx]->backward();
        }
    }
};

inline PExecute PoolNode::generate(bool bTrain, dtype cur_drop_factor) {
    PoolExecute* exec = new PoolExecute();
    exec->batch.push_back(this);
    exec->bTrain = bTrain;
    exec->drop_factor = cur_drop_factor;
    return exec;
}



class SumPoolNode : public Node {
  public:
    vector<PNode> ins;

    ~SumPoolNode() {
        ins.clear();
    }
  public:
    SumPoolNode() : Node() {
        ins.clear();
        node_type = "sum-pool";
    }

    inline void clearValue() {
        ins.clear();
        Node::clearValue();
    }

  public:
    void forward(Graph *cg, const vector<PNode>& x) {
        if (x.size() == 0) {
            std::cout << "empty inputs for add" << std::endl;
            return;
        }

        ins.clear();
        for (int i = 0; i < x.size(); i++) {
            if (x[i]->val.dim == dim) {
                ins.push_back(x[i]);
            } else {
                std::cout << "dim does not match" << std::endl;
            }
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1, PNode x2) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x2->dim == dim) {
            ins.push_back(x2);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1, PNode x2, PNode x3) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x2->dim == dim) {
            ins.push_back(x2);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x3->dim == dim) {
            ins.push_back(x3);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1, PNode x2, PNode x3, PNode x4) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x2->dim == dim) {
            ins.push_back(x2);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x3->dim == dim) {
            ins.push_back(x3);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x4->dim == dim) {
            ins.push_back(x4);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1, PNode x2, PNode x3, PNode x4, PNode x5) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x2->dim == dim) {
            ins.push_back(x2);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x3->dim == dim) {
            ins.push_back(x3);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x4->dim == dim) {
            ins.push_back(x4);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x5->dim == dim) {
            ins.push_back(x5);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1, PNode x2, PNode x3, PNode x4, PNode x5, PNode x6) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x2->dim == dim) {
            ins.push_back(x2);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x3->dim == dim) {
            ins.push_back(x3);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x4->dim == dim) {
            ins.push_back(x4);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x5->dim == dim) {
            ins.push_back(x5);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x6->dim == dim) {
            ins.push_back(x6);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

  public:
    inline void compute() {
        int nSize = ins.size();
        val.zero();
        for (int i = 0; i < nSize; ++i) {
            for (int idx = 0; idx < dim; idx++) {
                val[idx] += ins[i]->val[idx];
            }
        }
    }


    void backward() {
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            for (int idx = 0; idx < dim; idx++) {
                ins[i]->loss[idx] += loss[idx];
            }
        }
    }


  public:
    inline PExecute generate(bool bTrain, dtype cur_drop_factor);

    // better to rewrite for deep understanding
    inline bool typeEqual(PNode other) {
        return Node::typeEqual(other);
    }

};


class SumPoolExecute : public Execute {
  public:
    bool bTrain;
  public:
    inline void  forward() {
        int count = batch.size();
        //#pragma omp parallel for
        for (int idx = 0; idx < count; idx++) {
            batch[idx]->compute();
            batch[idx]->forward_drop(bTrain, drop_factor);
        }
    }

    inline void backward() {
        int count = batch.size();
        //#pragma omp parallel for
        for (int idx = 0; idx < count; idx++) {
            batch[idx]->backward_drop();
            batch[idx]->backward();
        }
    }
};


inline PExecute SumPoolNode::generate(bool bTrain, dtype cur_drop_factor) {
    SumPoolExecute* exec = new SumPoolExecute();
    exec->batch.push_back(this);
    exec->bTrain = bTrain;
    exec->drop_factor = cur_drop_factor;
    return exec;
}



class AvgPoolNode : public Node {
  public:
    vector<PNode> ins;

    ~AvgPoolNode() {
        ins.clear();
    }
  public:
    AvgPoolNode() : Node() {
        ins.clear();
        node_type = "avg-pool";
    }

    inline void clearValue() {
        ins.clear();
        Node::clearValue();
    }

  public:
    void forward(Graph *cg, const vector<PNode>& x) {
        if (x.size() == 0) {
            std::cout << "empty inputs for add" << std::endl;
            return;
        }

        ins.clear();
        for (int i = 0; i < x.size(); i++) {
            if (x[i]->val.dim == dim) {
                ins.push_back(x[i]);
            } else {
                std::cout << "dim does not match" << std::endl;
            }
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1, PNode x2) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x2->dim == dim) {
            ins.push_back(x2);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1, PNode x2, PNode x3) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x2->dim == dim) {
            ins.push_back(x2);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x3->dim == dim) {
            ins.push_back(x3);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1, PNode x2, PNode x3, PNode x4) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x2->dim == dim) {
            ins.push_back(x2);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x3->dim == dim) {
            ins.push_back(x3);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x4->dim == dim) {
            ins.push_back(x4);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1, PNode x2, PNode x3, PNode x4, PNode x5) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x2->dim == dim) {
            ins.push_back(x2);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x3->dim == dim) {
            ins.push_back(x3);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x4->dim == dim) {
            ins.push_back(x4);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x5->dim == dim) {
            ins.push_back(x5);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

    void forward(Graph *cg, PNode x1, PNode x2, PNode x3, PNode x4, PNode x5, PNode x6) {
        ins.clear();
        if (x1->dim == dim) {
            ins.push_back(x1);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x2->dim == dim) {
            ins.push_back(x2);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x3->dim == dim) {
            ins.push_back(x3);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x4->dim == dim) {
            ins.push_back(x4);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x5->dim == dim) {
            ins.push_back(x5);
        } else {
            std::cout << "dim does not match" << std::endl;
        }
        if (x6->dim == dim) {
            ins.push_back(x6);
        } else {
            std::cout << "dim does not match" << std::endl;
        }

        degree = 0;
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            ins[i]->addParent(this);
        }

        cg->addNode(this);
    }

  public:
    inline void compute() {
        int nSize = ins.size();
        val.zero();
        for (int i = 0; i < nSize; ++i) {
            for (int idx = 0; idx < dim; idx++) {
                val[idx] += ins[i]->val[idx] * 1.0 / nSize;
            }
        }

    }


    void backward() {
        int nSize = ins.size();
        for (int i = 0; i < nSize; ++i) {
            for (int idx = 0; idx < dim; idx++) {
                ins[i]->loss[idx] += loss[idx] * 1.0 / nSize;
            }
        }
    }


  public:
    inline PExecute generate(bool bTrain, dtype cur_drop_factor);

    // better to rewrite for deep understanding
    inline bool typeEqual(PNode other) {
        return Node::typeEqual(other);
    }

};


class AvgPoolExecute : public Execute {
  public:
    bool bTrain;
  public:
    inline void  forward() {
        int count = batch.size();
        //#pragma omp parallel for
        for (int idx = 0; idx < count; idx++) {
            batch[idx]->compute();
            batch[idx]->forward_drop(bTrain, drop_factor);
        }
    }

    inline void backward() {
        int count = batch.size();
        //#pragma omp parallel for
        for (int idx = 0; idx < count; idx++) {
            batch[idx]->backward_drop();
            batch[idx]->backward();
        }
    }
};


inline PExecute AvgPoolNode::generate(bool bTrain, dtype cur_drop_factor) {
    AvgPoolExecute* exec = new AvgPoolExecute();
    exec->batch.push_back(this);
    exec->bTrain = bTrain;
    exec->drop_factor = cur_drop_factor;
    return exec;
}

#endif
