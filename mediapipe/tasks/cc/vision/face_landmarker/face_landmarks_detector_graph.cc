/* Copyright 2023 The MediaPipe Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "mediapipe/calculators/core/split_vector_calculator.pb.h"
#include "mediapipe/calculators/tensor/tensors_to_floats_calculator.pb.h"
#include "mediapipe/calculators/tensor/tensors_to_landmarks_calculator.pb.h"
#include "mediapipe/calculators/util/detections_to_rects_calculator.pb.h"
#include "mediapipe/calculators/util/rect_transformation_calculator.pb.h"
#include "mediapipe/calculators/util/thresholding_calculator.pb.h"
#include "mediapipe/framework/api2/builder.h"
#include "mediapipe/framework/api2/port.h"
#include "mediapipe/framework/formats/image.h"
#include "mediapipe/framework/formats/landmark.pb.h"
#include "mediapipe/framework/formats/rect.pb.h"
#include "mediapipe/framework/formats/tensor.h"
#include "mediapipe/tasks/cc/common.h"
#include "mediapipe/tasks/cc/components/processors/image_preprocessing_graph.h"
#include "mediapipe/tasks/cc/components/utils/gate.h"
#include "mediapipe/tasks/cc/core/model_resources.h"
#include "mediapipe/tasks/cc/core/model_task_graph.h"
#include "mediapipe/tasks/cc/core/utils.h"
#include "mediapipe/tasks/cc/vision/face_landmarker/proto/face_landmarks_detector_graph_options.pb.h"
#include "mediapipe/tasks/cc/vision/face_landmarker/proto/tensors_to_face_landmarks_graph_options.pb.h"
#include "mediapipe/tasks/cc/vision/utils/image_tensor_specs.h"

namespace mediapipe {
namespace tasks {
namespace vision {
namespace face_landmarker {

namespace {

using ::mediapipe::NormalizedRect;
using ::mediapipe::api2::Input;
using ::mediapipe::api2::Output;
using ::mediapipe::api2::builder::Graph;
using ::mediapipe::api2::builder::Stream;
using ::mediapipe::tasks::components::utils::AllowIf;

constexpr char kImageTag[] = "IMAGE";
constexpr char kNormRectTag[] = "NORM_RECT";
constexpr char kFaceRectNextFrameTag[] = "FACE_RECT_NEXT_FRAME";
constexpr char kFaceRectsNextFrameTag[] = "FACE_RECTS_NEXT_FRAME";
constexpr char kPresenceTag[] = "PRESENCE";
constexpr char kPresenceScoreTag[] = "PRESENCE_SCORE";
constexpr char kImageSizeTag[] = "IMAGE_SIZE";
constexpr char kTensorsTag[] = "TENSORS";
constexpr char kLandmarksTag[] = "LANDMARKS";
constexpr char kNormLandmarksTag[] = "NORM_LANDMARKS";
constexpr char kFloatTag[] = "FLOAT";
constexpr char kFlagTag[] = "FLAG";
constexpr char kLetterboxPaddingTag[] = "LETTERBOX_PADDING";
constexpr char kCloneTag[] = "CLONE";
constexpr char kIterableTag[] = "ITERABLE";
constexpr char kBatchEndTag[] = "BATCH_END";
constexpr char kItemTag[] = "ITEM";
constexpr char kDetectionTag[] = "DETECTION";

// a landmarks tensor and a scores tensor
constexpr int kFaceLandmarksOutputTensorsNum = 2;
// 6 landmarks tensors and a scores tensor.
constexpr int kAttentionMeshOutputTensorsNum = 7;

struct SingleFaceLandmarksOutputs {
  Stream<NormalizedLandmarkList> landmarks;
  Stream<NormalizedRect> rect_next_frame;
  Stream<bool> presence;
  Stream<float> presence_score;
};

struct MultiFaceLandmarksOutputs {
  Stream<std::vector<NormalizedLandmarkList>> landmarks_lists;
  Stream<std::vector<NormalizedRect>> rects_next_frame;
  Stream<std::vector<bool>> presences;
  Stream<std::vector<float>> presence_scores;
};

absl::Status SanityCheckOptions(
    const proto::FaceLandmarksDetectorGraphOptions& options) {
  if (options.min_detection_confidence() < 0 ||
      options.min_detection_confidence() > 1) {
    return CreateStatusWithPayload(absl::StatusCode::kInvalidArgument,
                                   "Invalid `min_detection_confidence` option: "
                                   "value must be in the range [0.0, 1.0]",
                                   MediaPipeTasksStatus::kInvalidArgumentError);
  }
  return absl::OkStatus();
}

// Split face landmark detection model output tensor into two parts,
// representing landmarks and face presence scores.
void ConfigureSplitTensorVectorCalculator(
    bool is_attention_model, mediapipe::SplitVectorCalculatorOptions* options) {
  if (is_attention_model) {
    auto* range = options->add_ranges();
    range->set_begin(0);
    range->set_end(kAttentionMeshOutputTensorsNum - 1);
    range = options->add_ranges();
    range->set_begin(kAttentionMeshOutputTensorsNum - 1);
    range->set_end(kAttentionMeshOutputTensorsNum);
  } else {
    auto* range = options->add_ranges();
    range->set_begin(0);
    range->set_end(kFaceLandmarksOutputTensorsNum - 1);
    range = options->add_ranges();
    range->set_begin(kFaceLandmarksOutputTensorsNum - 1);
    range->set_end(kFaceLandmarksOutputTensorsNum);
  }
}

void ConfigureTensorsToFaceLandmarksGraph(
    const ImageTensorSpecs& input_image_tensor_spec, bool is_attention_model,
    proto::TensorsToFaceLandmarksGraphOptions* options) {
  options->set_is_attention_model(is_attention_model);
  options->set_input_image_height(input_image_tensor_spec.image_height);
  options->set_input_image_width(input_image_tensor_spec.image_width);
}

void ConfigureFaceDetectionsToRectsCalculator(
    mediapipe::DetectionsToRectsCalculatorOptions* options) {
  // Left side of left eye.
  options->set_rotation_vector_start_keypoint_index(33);
  // Right side of right eye.
  options->set_rotation_vector_end_keypoint_index(263);
  options->set_rotation_vector_target_angle_degrees(0);
}

void ConfigureFaceRectTransformationCalculator(
    mediapipe::RectTransformationCalculatorOptions* options) {
  // TODO: make rect transformation configurable, e.g. from
  // Metadata or configuration options.
  options->set_scale_x(1.5f);
  options->set_scale_y(1.5f);
  options->set_square_long(true);
}

bool IsAttentionModel(const core::ModelResources& model_resources) {
  const auto* model = model_resources.GetTfLiteModel();
  const auto* primary_subgraph = (*model->subgraphs())[0];
  return primary_subgraph->outputs()->size() == kAttentionMeshOutputTensorsNum;
}

}  // namespace

// A "mediapipe.tasks.vision.face_landmarker.SingleFaceLandmarksDetectorGraph"
// performs face landmarks detection.
//
// Inputs:
//   IMAGE - Image
//     Image to perform detection on.
//   NORM_RECT - NormalizedRect @Optional
//     Rect enclosing the RoI to perform detection on. If not set, the detection
//     RoI is the whole image.
//
//
// Outputs:
//   NORM_LANDMARKS: - NormalizedLandmarkList
//     Detected face landmarks.
//   FACE_RECT_NEXT_FRAME - NormalizedRect
//     The predicted Rect enclosing the face RoI for landmark detection on the
//     next frame.
//   PRESENCE - bool
//     Boolean value indicates whether the face is present.
//   PRESENCE_SCORE - float
//     Float value indicates the probability that the face is present.
//
// Example:
// node {
//   calculator:
//   "mediapipe.tasks.vision.face_landmarker.SingleFaceLandmarksDetectorGraph"
//   input_stream: "IMAGE:input_image"
//   input_stream: "FACE_RECT:face_rect"
//   output_stream: "LANDMARKS:face_landmarks"
//   output_stream: "FACE_RECT_NEXT_FRAME:face_rect_next_frame"
//   output_stream: "PRESENCE:presence"
//   output_stream: "PRESENCE_SCORE:presence_score"
//   options {
//     [mediapipe.tasks.vision.face_landmarker.proto.FaceLandmarksDetectorGraphOptions.ext]
//     {
//       base_options {
//          model_asset {
//            file_name: "face_landmark_lite.tflite"
//          }
//       }
//       min_detection_confidence: 0.5
//     }
//   }
// }
class SingleFaceLandmarksDetectorGraph : public core::ModelTaskGraph {
 public:
  absl::StatusOr<CalculatorGraphConfig> GetConfig(
      SubgraphContext* sc) override {
    ASSIGN_OR_RETURN(
        const auto* model_resources,
        CreateModelResources<proto::FaceLandmarksDetectorGraphOptions>(sc));
    Graph graph;
    ASSIGN_OR_RETURN(
        auto outs,
        BuildSingleFaceLandmarksDetectorGraph(
            sc->Options<proto::FaceLandmarksDetectorGraphOptions>(),
            *model_resources, graph[Input<Image>(kImageTag)],
            graph[Input<NormalizedRect>::Optional(kNormRectTag)], graph));
    outs.landmarks >>
        graph.Out(kNormLandmarksTag).Cast<NormalizedLandmarkList>();
    outs.rect_next_frame >>
        graph.Out(kFaceRectNextFrameTag).Cast<NormalizedRect>();
    outs.presence >> graph.Out(kPresenceTag).Cast<bool>();
    outs.presence_score >> graph.Out(kPresenceScoreTag).Cast<float>();
    return graph.GetConfig();
  }

 private:
  // Adds a mediapipe face landmark detection graph into the provided
  // builder::Graph instance.
  //
  // subgraph_options: the mediapipe tasks module
  //   FaceLandmarksDetectorGraphOptions.
  // model_resources: the ModelSources object initialized from a face landmark
  //   detection model file with model metadata.
  // image_in: (mediapipe::Image) stream to run face landmark detection on.
  // face_rect: (NormalizedRect) stream to run on the RoI of image.
  // graph: the mediapipe graph instance to be updated.
  absl::StatusOr<SingleFaceLandmarksOutputs>
  BuildSingleFaceLandmarksDetectorGraph(
      const proto::FaceLandmarksDetectorGraphOptions& subgraph_options,
      const core::ModelResources& model_resources, Stream<Image> image_in,
      Stream<NormalizedRect> face_rect, Graph& graph) {
    MP_RETURN_IF_ERROR(SanityCheckOptions(subgraph_options));

    auto& preprocessing = graph.AddNode(
        "mediapipe.tasks.components.processors.ImagePreprocessingGraph");
    bool use_gpu =
        components::processors::DetermineImagePreprocessingGpuBackend(
            subgraph_options.base_options().acceleration());
    MP_RETURN_IF_ERROR(components::processors::ConfigureImagePreprocessingGraph(
        model_resources, use_gpu,
        &preprocessing.GetOptions<tasks::components::processors::proto::
                                      ImagePreprocessingGraphOptions>()));
    image_in >> preprocessing.In(kImageTag);
    face_rect >> preprocessing.In(kNormRectTag);
    auto image_size = preprocessing.Out(kImageSizeTag);
    auto letterbox_padding = preprocessing.Out(kLetterboxPaddingTag);
    auto input_tensors = preprocessing.Out(kTensorsTag);

    auto& inference = AddInference(
        model_resources, subgraph_options.base_options().acceleration(), graph);
    input_tensors >> inference.In(kTensorsTag);
    auto output_tensors = inference.Out(kTensorsTag);

    // Split model output tensors to multiple streams.
    bool is_attention_model = IsAttentionModel(model_resources);
    auto& split_tensors_vector = graph.AddNode("SplitTensorVectorCalculator");
    ConfigureSplitTensorVectorCalculator(
        is_attention_model,
        &split_tensors_vector
             .GetOptions<mediapipe::SplitVectorCalculatorOptions>());
    output_tensors >> split_tensors_vector.In("");
    auto landmark_tensors = split_tensors_vector.Out(0);
    auto presence_flag_tensors = split_tensors_vector.Out(1);

    // Decodes the landmark tensors into a list of landmarks, where the landmark
    // coordinates are normalized by the size of the input image to the model.
    ASSIGN_OR_RETURN(auto image_tensor_specs,
                     vision::BuildInputImageTensorSpecs(model_resources));
    auto& tensors_to_face_landmarks = graph.AddNode(
        "mediapipe.tasks.vision.face_landmarker.TensorsToFaceLandmarksGraph");
    ConfigureTensorsToFaceLandmarksGraph(
        image_tensor_specs, is_attention_model,
        &tensors_to_face_landmarks
             .GetOptions<proto::TensorsToFaceLandmarksGraphOptions>());
    landmark_tensors >> tensors_to_face_landmarks.In(kTensorsTag);
    auto landmarks = tensors_to_face_landmarks.Out(kNormLandmarksTag);

    // Converts the presence flag tensor into a float that represents the
    // confidence score of face presence.
    auto& tensors_to_presence = graph.AddNode("TensorsToFloatsCalculator");
    tensors_to_presence
        .GetOptions<mediapipe::TensorsToFloatsCalculatorOptions>()
        .set_activation(mediapipe::TensorsToFloatsCalculatorOptions::SIGMOID);
    presence_flag_tensors >> tensors_to_presence.In(kTensorsTag);
    auto presence_score = tensors_to_presence.Out(kFloatTag).Cast<float>();

    // Applies a threshold to the confidence score to determine whether a
    // face is present.
    auto& presence_thresholding = graph.AddNode("ThresholdingCalculator");
    presence_thresholding.GetOptions<mediapipe::ThresholdingCalculatorOptions>()
        .set_threshold(subgraph_options.min_detection_confidence());
    presence_score >> presence_thresholding.In(kFloatTag);
    auto presence = presence_thresholding.Out(kFlagTag).Cast<bool>();

    // Adjusts landmarks (already normalized to [0.f, 1.f]) on the letterboxed
    // face image (after image transformation with the FIT scale mode) to the
    // corresponding locations on the same image with the letterbox removed
    // (face image before image transformation).
    auto& landmark_letterbox_removal =
        graph.AddNode("LandmarkLetterboxRemovalCalculator");
    letterbox_padding >> landmark_letterbox_removal.In(kLetterboxPaddingTag);
    landmarks >> landmark_letterbox_removal.In(kLandmarksTag);
    auto landmarks_letterbox_removed =
        landmark_letterbox_removal.Out(kLandmarksTag);

    // Projects the landmarks from the cropped face image to the corresponding
    // locations on the full image before cropping (input to the graph).
    auto& landmark_projection = graph.AddNode("LandmarkProjectionCalculator");
    landmarks_letterbox_removed >> landmark_projection.In(kNormLandmarksTag);
    face_rect >> landmark_projection.In(kNormRectTag);
    auto projected_landmarks = AllowIf(
        landmark_projection[Output<NormalizedLandmarkList>(kNormLandmarksTag)],
        presence, graph);

    // Converts the face landmarks into a rectangle (normalized by image size)
    // that encloses the face.
    auto& landmarks_to_detection =
        graph.AddNode("LandmarksToDetectionCalculator");
    projected_landmarks >> landmarks_to_detection.In(kNormLandmarksTag);
    auto face_landmarks_detection = landmarks_to_detection.Out(kDetectionTag);
    auto& detection_to_rect = graph.AddNode("DetectionsToRectsCalculator");
    ConfigureFaceDetectionsToRectsCalculator(
        &detection_to_rect
             .GetOptions<mediapipe::DetectionsToRectsCalculatorOptions>());
    face_landmarks_detection >> detection_to_rect.In(kDetectionTag);
    image_size >> detection_to_rect.In(kImageSizeTag);
    auto face_landmarks_rect = detection_to_rect.Out(kNormRectTag);

    // Expands the face rectangle so that in the next video frame it's likely to
    // still contain the face even with some motion.
    auto& face_rect_transformation =
        graph.AddNode("RectTransformationCalculator");
    ConfigureFaceRectTransformationCalculator(
        &face_rect_transformation
             .GetOptions<mediapipe::RectTransformationCalculatorOptions>());
    image_size >> face_rect_transformation.In(kImageSizeTag);
    face_landmarks_rect >> face_rect_transformation.In(kNormRectTag);
    auto face_rect_next_frame =
        AllowIf(face_rect_transformation.Out("").Cast<NormalizedRect>(),
                presence, graph);
    return {{
        /* landmarks= */ projected_landmarks,
        /* rect_next_frame= */ face_rect_next_frame,
        /* presence= */ presence,
        /* presence_score= */ presence_score,
    }};
  }
};

// clang-format off
REGISTER_MEDIAPIPE_GRAPH(
  ::mediapipe::tasks::vision::face_landmarker::SingleFaceLandmarksDetectorGraph); // NOLINT
// clang-format on

// A "mediapipe.tasks.vision.face_landmarker.MultiFaceLandmarksDetectorGraph"
// performs multi face landmark detection.
// - Accepts an input image and a vector of face rect RoIs to detect the
//   multiple face landmarks enclosed by the RoIs. Output vectors of
//   face landmarks related results, where each element in the vectors
//   corrresponds to the result of the same face.
//
// Inputs:
//   IMAGE - Image
//     Image to perform detection on.
//   NORM_RECT - std::vector<NormalizedRect>
//     A vector of multiple norm rects enclosing the face RoI to perform
//     landmarks detection on.
//
//
// Outputs:
//   LANDMARKS: - std::vector<NormalizedLandmarkList>
//     Vector of detected face landmarks.
//   FACE_RECTS_NEXT_FRAME - std::vector<NormalizedRect>
//     Vector of the predicted rects enclosing the same face RoI for landmark
//     detection on the next frame.
//   PRESENCE - std::vector<bool>
//     Vector of boolean value indicates whether the face is present.
//   PRESENCE_SCORE - std::vector<float>
//     Vector of float value indicates the probability that the face is present.
//
// Example:
// node {
//   calculator:
//   "mediapipe.tasks.vision.face_landmarker.MultiFaceLandmarksDetectorGraph"
//   input_stream: "IMAGE:input_image"
//   input_stream: "NORM_RECT:norm_rect"
//   output_stream: "LANDMARKS:landmarks"
//   output_stream: "FACE_RECTS_NEXT_FRAME:face_rects_next_frame"
//   output_stream: "PRESENCE:presence"
//   output_stream: "PRESENCE_SCORE:presence_score"
//   options {
//     [mediapipe.tasks.vision.face_landmarker.proto.FaceLandmarksDetectorGraphOptions.ext]
//     {
//       base_options {
//          model_asset {
//            file_name: "face_landmark_lite.tflite"
//          }
//       }
//       min_detection_confidence: 0.5
//     }
//   }
// }
class MultiFaceLandmarksDetectorGraph : public core::ModelTaskGraph {
 public:
  absl::StatusOr<CalculatorGraphConfig> GetConfig(
      SubgraphContext* sc) override {
    Graph graph;
    ASSIGN_OR_RETURN(
        auto outs,
        BuildFaceLandmarksDetectorGraph(
            sc->Options<proto::FaceLandmarksDetectorGraphOptions>(),
            graph[Input<Image>(kImageTag)],
            graph[Input<std::vector<NormalizedRect>>(kNormRectTag)], graph));
    outs.landmarks_lists >> graph.Out(kNormLandmarksTag)
                                .Cast<std::vector<NormalizedLandmarkList>>();
    outs.rects_next_frame >>
        graph.Out(kFaceRectsNextFrameTag).Cast<std::vector<NormalizedRect>>();
    outs.presences >> graph.Out(kPresenceTag).Cast<std::vector<bool>>();
    outs.presence_scores >>
        graph.Out(kPresenceScoreTag).Cast<std::vector<float>>();

    return graph.GetConfig();
  }

 private:
  absl::StatusOr<MultiFaceLandmarksOutputs> BuildFaceLandmarksDetectorGraph(
      const proto::FaceLandmarksDetectorGraphOptions& subgraph_options,
      Stream<Image> image_in,
      Stream<std::vector<NormalizedRect>> multi_face_rects, Graph& graph) {
    auto& face_landmark_subgraph = graph.AddNode(
        "mediapipe.tasks.vision.face_landmarker."
        "SingleFaceLandmarksDetectorGraph");
    face_landmark_subgraph
        .GetOptions<proto::FaceLandmarksDetectorGraphOptions>()
        .CopyFrom(subgraph_options);

    auto& begin_loop_multi_face_rects =
        graph.AddNode("BeginLoopNormalizedRectCalculator");

    image_in >> begin_loop_multi_face_rects.In(kCloneTag);
    multi_face_rects >> begin_loop_multi_face_rects.In(kIterableTag);
    auto batch_end = begin_loop_multi_face_rects.Out(kBatchEndTag);
    auto image = begin_loop_multi_face_rects.Out(kCloneTag);
    auto face_rect = begin_loop_multi_face_rects.Out(kItemTag);

    image >> face_landmark_subgraph.In(kImageTag);
    face_rect >> face_landmark_subgraph.In(kNormRectTag);
    auto presence = face_landmark_subgraph.Out(kPresenceTag);
    auto presence_score = face_landmark_subgraph.Out(kPresenceScoreTag);
    auto face_rect_next_frame =
        face_landmark_subgraph.Out(kFaceRectNextFrameTag);
    auto landmarks = face_landmark_subgraph.Out(kNormLandmarksTag);

    auto& end_loop_presence = graph.AddNode("EndLoopBooleanCalculator");
    batch_end >> end_loop_presence.In(kBatchEndTag);
    presence >> end_loop_presence.In(kItemTag);
    auto presences =
        end_loop_presence.Out(kIterableTag).Cast<std::vector<bool>>();

    auto& end_loop_presence_score = graph.AddNode("EndLoopFloatCalculator");
    batch_end >> end_loop_presence_score.In(kBatchEndTag);
    presence_score >> end_loop_presence_score.In(kItemTag);
    auto presence_scores =
        end_loop_presence_score.Out(kIterableTag).Cast<std::vector<float>>();

    auto& end_loop_landmarks =
        graph.AddNode("EndLoopNormalizedLandmarkListVectorCalculator");
    batch_end >> end_loop_landmarks.In(kBatchEndTag);
    landmarks >> end_loop_landmarks.In(kItemTag);
    auto landmark_lists = end_loop_landmarks.Out(kIterableTag)
                              .Cast<std::vector<NormalizedLandmarkList>>();

    auto& end_loop_rects_next_frame =
        graph.AddNode("EndLoopNormalizedRectCalculator");
    batch_end >> end_loop_rects_next_frame.In(kBatchEndTag);
    face_rect_next_frame >> end_loop_rects_next_frame.In(kItemTag);
    auto face_rects_next_frame = end_loop_rects_next_frame.Out(kIterableTag)
                                     .Cast<std::vector<NormalizedRect>>();

    return {{
        /* landmarks_lists= */ landmark_lists,
        /* face_rects_next_frame= */ face_rects_next_frame,
        /* presences= */ presences,
        /* presence_scores= */ presence_scores,
    }};
  }
};

// clang-format off
REGISTER_MEDIAPIPE_GRAPH(
  ::mediapipe::tasks::vision::face_landmarker::MultiFaceLandmarksDetectorGraph);
  // NOLINT
// clang-format on

}  // namespace face_landmarker
}  // namespace vision
}  // namespace tasks
}  // namespace mediapipe
