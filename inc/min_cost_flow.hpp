#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace procon37 {

// 小さい割当ネットワーク用。負の報酬辺と残余辺を含めて最短増加路を求める。
// 呼び出し側は初期グラフに負閉路を作らないこと。
class MinCostFlow {
public:
    struct Edge { int to; int reverse; int capacity; std::int64_t cost; };

    explicit MinCostFlow(int vertices) : graph_(static_cast<std::size_t>(vertices)) {}

    // 辺配列の確保領域を再利用し、新しいネットワークとして初期化する。
    void reset(int vertices) {
        graph_.resize(static_cast<std::size_t>(vertices));
        for (auto& edges : graph_) edges.clear();
    }

    void reserveEdges(int vertex, std::size_t count) { graph_[vertex].reserve(count); }

    int addEdge(int from, int to, int capacity, std::int64_t cost) {
        const int index = static_cast<int>(graph_[from].size());
        const int reverse = static_cast<int>(graph_[to].size()) + (from == to ? 1 : 0);
        graph_[from].push_back({to, reverse, capacity, cost});
        graph_[to].push_back({from, index, 0, -cost});
        return index;
    }

    int capacity(int from, int edge) const { return graph_[from][edge].capacity; }

    std::pair<int, std::int64_t> run(int source, int sink, int limit) {
        const int n = static_cast<int>(graph_.size());
        const auto inf = std::numeric_limits<std::int64_t>::max() / 4;
        distances_.resize(n);
        previous_.resize(n);
        edges_.resize(n);
        queued_.resize(n);
        queue_.resize(static_cast<std::size_t>(n) + 1);
        auto& dist = distances_;
        auto& previous = previous_;
        auto& edge = edges_;
        auto& queued = queued_;
        int flow = 0;
        std::int64_t cost = 0;
        while (flow < limit) {
            std::fill(dist.begin(), dist.end(), inf);
            std::fill(queued.begin(), queued.end(), false);
            // 同時にqueuedになる頂点は最大n個。固定容量FIFOで
            // std::dequeのブロック割当を各増加路から取り除く。
            std::size_t head = 0, tail = 0;
            auto push = [&](int vertex) {
                queue_[tail] = vertex;
                if (++tail == queue_.size()) tail = 0;
            };
            dist[source] = 0;
            push(source);
            queued[source] = true;
            while (head != tail) {
                const int v = queue_[head];
                if (++head == queue_.size()) head = 0;
                queued[v] = false;
                for (int i = 0; i < static_cast<int>(graph_[v].size()); ++i) {
                    const Edge& e = graph_[v][i];
                    if (e.capacity <= 0 || dist[e.to] <= dist[v] + e.cost) continue;
                    dist[e.to] = dist[v] + e.cost;
                    previous[e.to] = v;
                    edge[e.to] = i;
                    if (!queued[e.to]) {
                        queued[e.to] = true;
                        push(e.to);
                    }
                }
            }
            if (dist[sink] == inf) break;
            int amount = limit - flow;
            for (int v = sink; v != source; v = previous[v])
                amount = std::min(amount, graph_[previous[v]][edge[v]].capacity);
            for (int v = sink; v != source; v = previous[v]) {
                Edge& e = graph_[previous[v]][edge[v]];
                e.capacity -= amount;
                graph_[v][e.reverse].capacity += amount;
            }
            flow += amount;
            cost += amount * dist[sink];
        }
        return {flow, cost};
    }

private:
    std::vector<std::vector<Edge>> graph_;
    std::vector<std::int64_t> distances_;
    std::vector<int> previous_, edges_, queue_;
    std::vector<std::uint8_t> queued_;
};

}  // namespace procon37
