// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <blockfilter.h>

static void ConstructGCSFilter(benchmark::Bench& bench)
{
    GCSFilter::ElementSet elements;
    for (int i = 0; i < 10000; ++i) {
        GCSFilter::Element element(32);
        element[0] = static_cast<unsigned char>(i);
        element[1] = static_cast<unsigned char>(i >> 8);
        elements.insert(std::move(element));
    }

    uint64_t siphash_k0 = 0;
    bench.batch(elements.size()).unit("elem").run([&] {
        GCSFilter filter({siphash_k0, 0, 20, 1 << 20}, elements);

        siphash_k0++;
    });
}

static void MatchGCSFilter(benchmark::Bench& bench)
{
    GCSFilter::ElementSet elements;
    for (int i = 0; i < 10000; ++i) {
        GCSFilter::Element element(32);
        element[0] = static_cast<unsigned char>(i);
        element[1] = static_cast<unsigned char>(i >> 8);
        elements.insert(std::move(element));
    }
    GCSFilter filter({0, 0, 20, 1 << 20}, elements);

    bench.unit("elem").run([&] {
        filter.Match(GCSFilter::Element());
    });
}

BENCHMARK(ConstructGCSFilter);
BENCHMARK(MatchGCSFilter);
