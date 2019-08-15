#include "Render.h"

#include "Primitive.h"
#include "Random2DSampler.h"
#include "Scene.h"
#include "math/Camera.h"
#include "util/ArrayOutput.h"
#include "util/Progressifier.h"

#include <future>
#include <range/v3/all.hpp>

namespace fp {

constexpr auto FirstBounceNumUSamples = 6;
constexpr auto FirstBounceNumVSamples = 3;

// What is functional?
// * Immutable data structures. e.g. "SampledPixel" in arrayOutput doesn't
//   accumulate, but some final process should. (if we do accumulation)
// * Scene graph using variant!
// * TMP stuff (if can find a way)

struct IntersectionRecord {
  Hit hit;
  Material material;
};

struct IntersectVisitor {
  const Ray &ray;

  static std::optional<IntersectionRecord>
  unwrapWith(const std::optional<Hit> &hit, const Material &m) {
    if (!hit)
      return {};
    return IntersectionRecord{*hit, m};
  }

  std::optional<IntersectionRecord>
  operator()(const TrianglePrimitive &primitive) const {
    return unwrapWith(primitive.triangle.intersect(ray), primitive.material);
  }
  std::optional<IntersectionRecord>
  operator()(const SpherePrimitive &primitive) const {
    return unwrapWith(primitive.sphere.intersect(ray), primitive.material);
  }
};

std::optional<IntersectionRecord> intersect(const Primitive &primitive,
                                            const Ray &ray) {
  return std::visit(IntersectVisitor{ray}, primitive);
}

std::optional<IntersectionRecord> intersect(const Scene &scene,
                                            const Ray &ray) {
  std::optional<IntersectionRecord> nearest;
  for (auto &primitive : scene.primitives) {
    auto thisIntersection = intersect(primitive, ray);
    if (thisIntersection
        && (!nearest || thisIntersection->hit.distance < nearest->hit.distance))
      nearest = thisIntersection;
  }
  return nearest;
}

Vec3 radiance(const Scene &scene, std::mt19937 &rng, const Ray &ray, int depth,
              int numUSamples, int numVSamples, bool preview);

Vec3 singleRay(const Scene &scene, std::mt19937 &rng,
               const IntersectionRecord &intersectionRecord, const Ray &ray,
               const OrthoNormalBasis &basis, double u, double v, int depth,
               bool preview) {
  std::uniform_real_distribution<> unit(0, 1.0);
  const auto &mat = intersectionRecord.material;
  const auto &hit = intersectionRecord.hit;
  const auto theta = 2 * M_PI * u;
  const auto radiusSquared = v;
  const auto radius = sqrt(radiusSquared);
  // Construct the new direction.
  const auto newDir =
      basis
          .transform(Vec3(cos(theta) * radius, sin(theta) * radius,
                          sqrt(1 - radiusSquared)))
          .normalised();
  const auto p = unit(rng);

  if (p < mat.reflectivity) {
    auto reflected =
        ray.direction() - hit.normal * 2 * hit.normal.dot(ray.direction());
    auto newRay = Ray::fromOriginAndDirection(hit.position, reflected);

    return radiance(scene, rng, newRay, depth, 1, 1, preview);
  } else {
    auto newRay = Ray::fromOriginAndDirection(hit.position, newDir);

    return radiance(scene, rng, newRay, depth, 1, 1, preview);
  }
}

// TODO rangev3?
static constexpr auto MaxDepth = 5;
Vec3 radiance(const Scene &scene, std::mt19937 &rng, const Ray &ray, int depth,
              int numUSamples, int numVSamples, bool preview) {
  if (depth >= MaxDepth)
    return Vec3();
  const auto intersectionRecord = intersect(scene, ray);
  if (!intersectionRecord)
    return scene.environment;

  const auto &mat = intersectionRecord->material;
  const auto &hit = intersectionRecord->hit;
  if (preview)
    return mat.diffuse;

  // Create a coordinate system local to the point, where the z is the
  // normal at this point.
  const auto basis = OrthoNormalBasis::fromZ(hit.normal);
  const auto sampleScale = 1.0 / (numUSamples * numVSamples);
  const auto range = Random2DSampler(rng, numUSamples, numVSamples);
  const Vec3 incomingLight = std::accumulate(
      std::begin(range), std::end(range), Vec3(),
      [&](Vec3 colour, const std::pair<double, double> &s) {
        return colour
               + singleRay(scene, rng, *intersectionRecord, ray, basis, s.first,
                           s.second, depth + 1, preview);
      });
  return mat.emission + mat.diffuse * incomingLight * sampleScale;
}

ArrayOutput renderWholeScreen(const Camera &camera, const Scene &scene,
                              size_t seed, int width, int height,
                              bool preview) {
  ArrayOutput result(width, height);
  std::mt19937 rng(seed); // TODO this is mutable shared state below
  using namespace ranges;
  auto xRange = view::ints(0, width) | view::cycle | view::take(width * height);
  auto yRange = view::for_each(view::ints(0, height), [width](int i) {
    return yield_from(view::repeat_n(i, width));
  });
  auto xyRange = view::zip(xRange, yRange);
  std::vector<Vec3> colours =
      xyRange
      | view::transform([width, height, &rng, &camera](auto tuple) {
        std::uniform_real_distribution<> unit(0, 1.0);
          auto [x, y] = tuple;
          auto u = unit(rng);
          auto v = unit(rng);
          auto yy = (2 * (static_cast<double>(y) + u + 0.5) / (height - 1)) - 1;
          auto xx = (2 * (static_cast<double>(x) + v + 0.5) / (width - 1)) - 1;
          return camera.ray(xx, yy, rng);
        })
      | view::transform([&scene, &rng, preview](const Ray &ray) {
          return radiance(scene, rng, ray, 0, FirstBounceNumUSamples,
                          FirstBounceNumVSamples, preview);
        });
  // TODO make an iterator that does this and pass it to the above!
  for (auto y = 0; y < height; ++y) {
    for (auto x = 0; x < width; ++x) {
      result.addSamples(x, y, colours[x + y * width], 1);
    }
  }
  return result;
};

void render(const Camera &camera, const Scene &scene, ArrayOutput &output,
            int samplesPerPixel, int numThreads, bool preview,
            const std::function<void()> &updateFunc) {
  std::uniform_real_distribution<> unit(0.0, 1.0);

  // TODO no raw loops...maybe return whole "Samples" of an entire screen and
  // accumulate separately? then feeds into a nice multithreaded future based
  // thing?
  // future from an async()
  size_t seed = 0;
  size_t numDone = 0;
  Progressifier progressifier(samplesPerPixel);
  for (int sample = 0; sample < samplesPerPixel; sample += numThreads) {
    std::vector<std::future<ArrayOutput>> futures;
    for (int ss = sample; ss < std::min(samplesPerPixel, sample + numThreads);
         ++ss) {
      futures.emplace_back(std::async(std::launch::async, [&] {
        return renderWholeScreen(camera, scene, seed++, output.width(),
                                 output.height(), preview);
      }));
    }
    for (auto &&future : futures) {
      future.wait();
      output += future.get();
      numDone++;
      progressifier.update(numDone);
      updateFunc();
    }
  }
}

}
