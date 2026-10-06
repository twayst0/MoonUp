// GPU time measurement with D3D11 timestamp queries (results read back a few frames later).
#pragma once
#include "d3d.h"

namespace sw {

class GpuTimer {
public:
    static constexpr int kSlots = 16;
    static constexpr int kMarks = 8;

    bool Init(ID3D11Device* dev) {
        D3D11_QUERY_DESC dq{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC tq{D3D11_QUERY_TIMESTAMP, 0};
        for (auto& s : slots_) {
            if (FAILED(dev->CreateQuery(&dq, &s.disjoint))) return false;
            for (auto& q : s.marks)
                if (FAILED(dev->CreateQuery(&tq, &q))) return false;
        }
        return true;
    }
    // Every Begin counts as one measured job. When all query slots are still in flight (high rates)
    // the job is not timed, but counted: Estimate() extrapolates from the timed ones.
    void Begin(ID3D11DeviceContext* ctx) {
        calls_++;
        int next = (cur_ + 1) % kSlots;
        if (slots_[next].pending) {
            active_ = false;  // never reuse a query that is still in flight
            return;
        }
        active_ = true;
        cur_ = next;
        Slot& s = slots_[cur_];
        s.used = 0;
        ctx->Begin(s.disjoint.Get());
        Mark(ctx);
    }
    void Mark(ID3D11DeviceContext* ctx) {
        if (!active_) return;
        Slot& s = slots_[cur_];
        if (s.used < kMarks) ctx->End(s.marks[s.used++].Get());
    }
    void End(ID3D11DeviceContext* ctx) {
        if (!active_) return;
        Mark(ctx);
        Slot& s = slots_[cur_];
        ctx->End(s.disjoint.Get());
        s.pending = true;
        active_ = false;
    }
    // GPU time of all jobs since the last call (ms): timed average x number of jobs.
    double Estimate() {
        if (done_ > 0) avgMs_ = avgMs_ <= 0 ? totalMs_ / done_ : avgMs_ * 0.5 + (totalMs_ / done_) * 0.5;
        double est = avgMs_ * calls_;
        totalMs_ = 0;
        done_ = 0;
        calls_ = 0;
        return est;
    }
    double AverageMs() const { return avgMs_; }
    // Collects finished slots without stalling. Returns the most recent total in ms (or -1).
    double Collect(ID3D11DeviceContext* ctx) {
        double result = -1;
        for (int k = 1; k <= kSlots; k++) {
            Slot& s = slots_[(cur_ + k) % kSlots];
            if (!s.pending || s.used < 2) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
            if (ctx->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            UINT64 t0 = 0, t1 = 0;
            if (ctx->GetData(s.marks[0].Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            if (ctx->GetData(s.marks[s.used - 1].Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            s.pending = false;
            if (!dj.Disjoint && dj.Frequency > 0 && t1 >= t0) {
                result = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
                totalMs_ += result;
                done_++;
            }
        }
        return result;
    }

    // Sum of all measured intervals since the last call (ms) and how many there were.
    double TakeTotal(int* count = nullptr) {
        double t = totalMs_;
        if (count) *count = done_;
        totalMs_ = 0;
        done_ = 0;
        return t;
    }

private:
    double totalMs_ = 0, avgMs_ = 0;
    int done_ = 0, calls_ = 0;
    bool active_ = false;
    struct Slot {
        ComPtr<ID3D11Query> disjoint;
        ComPtr<ID3D11Query> marks[kMarks];
        int used = 0;
        bool pending = false;
    };
    Slot slots_[kSlots];
    int cur_ = 0;
};

}  // namespace sw
