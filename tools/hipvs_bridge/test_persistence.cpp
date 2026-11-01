#include "seekdb_cuvs_bridge.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
  if (argc != 3 || (std::strcmp(argv[1], "write") != 0 &&
                    std::strcmp(argv[1], "read") != 0)) return 2;
  const long count = 10000;
  const long dimension = 32;
  std::vector<float> vectors(count * dimension);
  for (long row = 0; row < count; ++row) {
    for (long column = 0; column < dimension; ++column) {
      vectors[row * dimension + column] = std::sin(float(row * 31 + column * 17));
    }
  }
  void *index = nullptr;
  if (std::strcmp(argv[1], "write") == 0) {
    index = seekdb_cuvs_build(vectors.data(), count, dimension);
    if (!index) return 3;
    if (seekdb_cuvs_serialize(index, argv[2]) != 0) {
      seekdb_cuvs_free(index);
      return 3;
    }
  } else {
    void *invalid = seekdb_cuvs_deserialize(argv[2], count - 1, dimension);
    if (invalid) { seekdb_cuvs_free(invalid); return 6; }
    invalid = seekdb_cuvs_deserialize(argv[2], count, dimension + 1);
    if (invalid) { seekdb_cuvs_free(invalid); return 7; }
    index = seekdb_cuvs_deserialize(argv[2], count, dimension);
    if (!index) return 4;
  }
  // Exercise the retained workspace across changes in both Top-K and batch size.
  // These are bridge correctness probes, not the contest's single-SQL QPS test.
  const long topks[] = {10, 100, 10, 100, 10};
  const long batches[] = {1, 1, 2, 2, 1};
  int result = 0;
  for (unsigned step = 0; step < sizeof(topks) / sizeof(topks[0]); ++step) {
    const long topk = topks[step];
    const long nq = batches[step];
    std::vector<unsigned int> ids(nq * topk, (unsigned int)count);
    std::vector<float> distances(nq * topk, -1.0f);
    const int search_rc = seekdb_cuvs_search(index, vectors.data(), nq, topk,
                                            ids.data(), distances.data());
    if (search_rc != 0) {
      std::fprintf(stderr, "workspace search failed: step=%u nq=%ld topk=%ld bridge_rc=%d\n",
                   step, nq, topk, search_rc);
      result = 5;
      break;
    }
    for (long query = 0; query < nq && result == 0; ++query) {
      std::vector<bool> seen(count, false);
      for (long rank = 0; rank < topk; ++rank) {
        const long offset = query * topk + rank;
        const unsigned int id = ids[offset];
        if (id >= (unsigned int)count || seen[id] ||
            !std::isfinite(distances[offset]) || distances[offset] < 0.0f) {
          result = 8;
          break;
        }
        seen[id] = true;
      }
      if (result == 0 && (ids[query * topk] != (unsigned int)query ||
                         std::fabs(distances[query * topk]) > 0.0001f)) {
        result = 5;
      }
    }
    if (result != 0) {
      std::fprintf(stderr, "workspace probe failed: step=%u nq=%ld topk=%ld rc=%d\n",
                   step, nq, topk, result);
      break;
    }
  }
  seekdb_cuvs_free(index);
  if (result != 0) return result;
  std::printf("%s: n=%ld workspace_steps=5 topk=10/100 nq=1/2 valid_unique_ids=1 self_match=1\n",
              argv[1], count);
  return 0;
}
