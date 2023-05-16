#include <limits>
#include <mutex>
#include <random>
#include <thread>

#include "glad/gl.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ugu/camera.h"
#include "ugu/image_io.h"
#include "ugu/inpaint/inpaint.h"
#include "ugu/point.h"
#include "ugu/registration/nonrigid.h"
#include "ugu/registration/rigid.h"
#include "ugu/renderable_mesh.h"
#include "ugu/renderer/gl/renderer.h"
#include "ugu/textrans/texture_transfer.h"
#include "ugu/timer.h"
#include "ugu/util/image_util.h"
#include "ugu/util/string_util.h"
// #define GL_SILENCE_DEPRECATION
// #if defined(IMGUI_IMPL_OPENGL_ES2)
// #include <GLES2/gl2.h>
// #endif
#include <GLFW/glfw3.h>  // Will drag system OpenGL headers

using namespace ugu;

namespace {

Eigen::Vector2d g_prev_cursor_pos;
Eigen::Vector2d g_cursor_pos;
Eigen::Vector2d g_mouse_l_pressed_pos;
Eigen::Vector2d g_mouse_l_released_pos;
Eigen::Vector2d g_mouse_m_pressed_pos;
Eigen::Vector2d g_mouse_m_released_pos;
Eigen::Vector2d g_mouse_r_pressed_pos;
Eigen::Vector2d g_mouse_r_released_pos;

std::string g_error_message;

std::string g_callback_message;
bool g_callback_finished = true;

bool g_to_process_drag_l = false;
bool g_to_process_drag_m = false;
bool g_to_process_drag_r = false;

uint32_t g_subwindow_id = ~0u;
uint32_t g_prev_subwindow_id = ~0u;

bool g_mouse_l_pressed = false;
bool g_mouse_m_pressed = false;
bool g_mouse_r_pressed = false;
const double drag_th = 0.0;
const double g_drag_point_pix_dist_th = 20.0;

double g_mouse_wheel_yoffset = 0.0;
bool g_to_process_wheel = false;

const uint32_t MAX_N_SPLIT_WIDTH = 2;

int g_width = 1920;
int g_height = 1080;

struct SplitViewInfo;
std::mutex views_mtx;
std::vector<SplitViewInfo> g_views;

Eigen::Vector3f default_clear_color = {0.45f, 0.55f, 0.60f};
Eigen::Vector3f default_wire_color = {0.1f, 0.1f, 0.1f};

// Global geometry info
std::vector<RenderableMeshPtr> g_meshes;
std::vector<std::string> g_mesh_names;
std::vector<std::string> g_mesh_paths;
// std::vector<BvhPtr<Eigen::Vector3f, Eigen::Vector3i>> g_bvhs;

struct CastRayResult {
  size_t min_geoid = ~0u;
  IntersectResult intersection;
};

std::unordered_map<RenderableMeshPtr, std::vector<CastRayResult>>
    g_selected_positions;

std::unordered_map<RenderableMeshPtr, Eigen::Affine3f> g_model_matrices;
std::unordered_map<RenderableMeshPtr, bool> g_update_bvh;

bool g_first_frame = true;

Eigen::Vector3f GetPos(const IntersectResult &intersection, uint32_t geoid) {
  const auto &mesh = g_meshes.at(geoid);
  const auto &trans = g_model_matrices.at(mesh);
  const auto &face = mesh->vertex_indices()[intersection.fid];
  const auto &v0 = mesh->vertices()[face[0]];
  const auto &v1 = mesh->vertices()[face[1]];
  const auto &v2 = mesh->vertices()[face[2]];
  Eigen::Vector3f p =
      intersection.u * (v1 - v0) + intersection.v * (v2 - v0) + v0;

  p = trans * p;
  return p;
}

Eigen::Vector3f GetPos(const CastRayResult &res) {
  return GetPos(res.intersection, static_cast<uint32_t>(res.min_geoid));
}

std::vector<Eigen::Vector3f> ExtractPos(
    const std::vector<CastRayResult> &results) {
  std::vector<Eigen::Vector3f> poss;

  for (const auto &res : results) {
    poss.push_back(GetPos(res));
  }
  return poss;
}

struct IcpData {
  RenderableMeshPtr src_mesh;
  std::vector<Eigen::Vector3f> src_points;
  std::vector<Eigen::Vector3f> dst_points;
  std::vector<Eigen::Vector3f> src_normals;
  std::vector<Eigen::Vector3f> dst_normals;
  std::vector<Eigen::Vector3i> dst_faces;
  IcpTerminateCriteria terminate_criteria;
  IcpCorrespCriteria corresp_criteria;
  IcpOutput output;
  bool with_scale = false;
  CorrespFinderPtr corresp_finder = nullptr;
  IcpCallbackFunc callback = nullptr;
  IcpCorrespType corresp_type = IcpCorrespType::kPointToPlane;
  IcpLossType loss_type = IcpLossType::kPointToPlane;
};

struct NonrigidIcpData {
  RenderableMeshPtr src_mesh;
  RenderableMeshPtr dst_mesh;

  bool check_self_itersection = false;
  float angle_rad_th = 0.65f;
  float dist_th = -1.f;
  int nn_num = 10;
  bool dst_check_geometry_border = false;
  bool src_check_geometry_border = false;

  double max_alpha = 10.0;
  double min_alpha = 0.1;
  double beta = 100.0;
  double gamma = 1.0;
  int step = 10;

  int max_internal_iter = 10;
  double min_frobenius_norm_diff = 2.0;
};

struct TextransData {
  RenderableMeshPtr src_mesh;
  RenderableMeshPtr dst_mesh;
  Eigen::Vector2i dst_size = {1024, 1024};
  TexTransNoCorrespOutput output;
};

enum class AlgorithmStatus { STARTED, RUNNING, HALTING };

IcpData g_icp_data;
NonrigidIcpData g_nonrigidicp_data;
TextransData g_textrans_data;
AlgorithmStatus g_icp_run = AlgorithmStatus::HALTING;
AlgorithmStatus g_nonrigidicp_run = AlgorithmStatus::HALTING;
AlgorithmStatus g_textrans_run = AlgorithmStatus::HALTING;
bool g_algorithm_process_finish = false;
Eigen::Affine3f g_icp_start_trans;
std::mutex icp_mtx, nonrigidicp_mtx, nonrigidicp_update_mtx, textrans_mtx;

void IcpProcessCallback(const IcpTerminateCriteria &terminate_criteria,
                        const IcpOutput &output) {
  g_callback_message = "ICP : " + std::to_string(output.loss_histroty.size()) +
                       " / " + std::to_string(terminate_criteria.iter_max) +
                       "   " + std::to_string(output.loss_histroty.back());

  std::cout << g_callback_message << std::endl;

  const auto &last_trans = g_icp_data.output.transform_histry.back();

  g_model_matrices[g_icp_data.src_mesh] =
      last_trans.cast<float>() * g_icp_start_trans;
}

void IcpFinishCallback(const Eigen::Affine3f &orignal_trans) {
  const auto &last_trans = g_icp_data.output.transform_histry.back();

  g_model_matrices[g_icp_data.src_mesh] =
      last_trans.cast<float>() * orignal_trans;
  g_update_bvh[g_icp_data.src_mesh] = true;

  g_callback_finished = true;
}

void IcpProcess() {
  std::lock_guard<std::mutex> lock(icp_mtx);
  if (g_icp_run == AlgorithmStatus::STARTED) {
    g_icp_run = AlgorithmStatus::RUNNING;

    g_icp_start_trans = g_model_matrices[g_icp_data.src_mesh];

    Timer timer;
    timer.Start();

    RigidIcp(
        g_icp_data.src_points, g_icp_data.dst_points, g_icp_data.src_normals,
        g_icp_data.dst_normals, g_icp_data.dst_faces, g_icp_data.corresp_type,
        g_icp_data.loss_type, g_icp_data.terminate_criteria,
        g_icp_data.corresp_criteria, g_icp_data.output, g_icp_data.with_scale,
        nullptr, g_icp_data.corresp_finder, -1, g_icp_data.callback);

    timer.End();
    g_callback_message =
        "ICP took " + std::to_string(timer.elapsed_msec() / 1000) + " sec.";

    std::cout << g_callback_message << std::endl;

    IcpFinishCallback(g_icp_start_trans);

    g_icp_run = AlgorithmStatus::HALTING;
  }
}

void NonrigidIcpProcess() {
  std::lock_guard<std::mutex> lock(nonrigidicp_mtx);
  if (g_nonrigidicp_run == AlgorithmStatus::STARTED) {
    g_nonrigidicp_run = AlgorithmStatus::RUNNING;

    Timer timer;
    timer.Start();

    ugu::NonRigidIcp nicp;
    nicp.SetSrc(*g_nonrigidicp_data.src_mesh.get(),
                g_model_matrices[g_nonrigidicp_data.src_mesh]);
    auto transed_dst_mesh = Mesh::Create(
        *std::static_pointer_cast<Mesh>(g_nonrigidicp_data.dst_mesh));
    transed_dst_mesh->Transform(g_model_matrices[g_nonrigidicp_data.dst_mesh]);
    nicp.SetDst(*transed_dst_mesh);

    nicp.Init(g_nonrigidicp_data.check_self_itersection,
              g_nonrigidicp_data.angle_rad_th,
              g_nonrigidicp_data.dst_check_geometry_border,
              g_nonrigidicp_data.src_check_geometry_border);

    nicp.SetCorrespDistTh(g_nonrigidicp_data.dist_th);
    nicp.SetCorrespNnNum(g_nonrigidicp_data.nn_num);

    std::vector<PointOnFace> src_landmarks;
    for (const auto &res :
         g_selected_positions.at(g_nonrigidicp_data.src_mesh)) {
      PointOnFace pof;
      pof.fid = res.intersection.fid;
      pof.u = res.intersection.u;
      pof.v = res.intersection.v;
      src_landmarks.push_back(pof);
    }
    std::vector<double> betas(src_landmarks.size(), g_nonrigidicp_data.beta);
    nicp.SetSrcLandmarks(src_landmarks, betas);
    nicp.SetDstLandmarkPositions(
        ExtractPos(g_selected_positions.at(g_nonrigidicp_data.dst_mesh)));

    auto update_mesh = [&](bool update_base = false) {
      ugu::MeshPtr deformed = Mesh::Create(*nicp.GetDeformedSrc());
      deformed->Transform(
          g_model_matrices[g_nonrigidicp_data.src_mesh].inverse());
      deformed->CalcNormal();

      auto fnum = static_cast<int>(
          g_nonrigidicp_data.src_mesh->vertex_indices().size());
      {
        std::lock_guard<std::mutex> lock_update(nonrigidicp_update_mtx);

        bool to_split_uv = g_nonrigidicp_data.src_mesh->HasIndepentUv();
        if (to_split_uv) {
          for (int i = 0; i < fnum; i++) {
            const auto &face = g_nonrigidicp_data.src_mesh->vertex_indices()[i];
            for (int j = 0; j < 3; j++) {
              uint32_t index = i * 3 + j;
              g_nonrigidicp_data.src_mesh->renderable_vertices[index].pos =
                  deformed->vertices()[face[j]];

              g_nonrigidicp_data.src_mesh->renderable_vertices[index].nor =
                  deformed->normals()[face[j]];
            }
          }
        } else {
          for (int i = 0; i < fnum; i++) {
            const auto &face = g_nonrigidicp_data.src_mesh->vertex_indices()[i];
            for (int j = 0; j < 3; j++) {
              g_nonrigidicp_data.src_mesh->renderable_vertices[face[j]].pos =
                  deformed->vertices()[face[j]];

              g_nonrigidicp_data.src_mesh->renderable_vertices[face[j]].nor =
                  deformed->normals()[face[j]];
            }
          }
        }
      }

      // OpenGL APIs MUST NOT BE CALLED IN SUB THREADS

      // g_nonrigidicp_data.src_mesh->UpdateMesh();

      if (update_base) {
        g_nonrigidicp_data.src_mesh->set_vertices(deformed->vertices());
        g_nonrigidicp_data.src_mesh->CalcNormal();
      }
    };

    for (int i = 1; i <= g_nonrigidicp_data.step; ++i) {
      double alpha =
          g_nonrigidicp_data.max_alpha -
          i * (g_nonrigidicp_data.max_alpha - g_nonrigidicp_data.min_alpha) /
              g_nonrigidicp_data.step;

      g_callback_message = "NonRigid-ICP : " + std::to_string(i) + " / " +
                           std::to_string(g_nonrigidicp_data.step) +
                           "  with alpha " + std::to_string(alpha);
      std::cout << g_callback_message << std::endl;

      nicp.Registrate(alpha, g_nonrigidicp_data.gamma,
                      g_nonrigidicp_data.max_internal_iter,
                      g_nonrigidicp_data.min_frobenius_norm_diff);

      update_mesh();
    }

    timer.End();
    g_callback_message = "NonRigid-ICP took " +
                         std::to_string(timer.elapsed_msec() / 1000) + " sec.";

    std::cout << g_callback_message << std::endl;

    update_mesh(true);

    // ugu::MeshPtr deformed = nicp.GetDeformedSrc();
    // deformed->Transform(
    //     g_model_matrices[g_nonrigidicp_data.src_mesh].inverse());

    // deformed->WriteObj("./", "out");

    g_update_bvh[g_nonrigidicp_data.src_mesh] = true;

    g_callback_finished = true;
    g_nonrigidicp_run = AlgorithmStatus::HALTING;
  }
}

void TextransProcess() {
  std::lock_guard<std::mutex> lock(textrans_mtx);
  if (g_textrans_run == AlgorithmStatus::STARTED) {
    g_textrans_run = AlgorithmStatus::RUNNING;

    Timer timer;
    timer.Start();

    Image3f src_tex;
    g_textrans_data.dst_mesh->materials()[0].diffuse_tex.convertTo(
        src_tex, CV_32FC3, 1.0, 0.0);
    TexTransNoCorresp(
        src_tex,
        *std::static_pointer_cast<Mesh>(g_textrans_data.dst_mesh).get(),
        g_model_matrices[g_textrans_data.dst_mesh],
        *std::static_pointer_cast<Mesh>(g_textrans_data.src_mesh).get(),
        g_model_matrices[g_textrans_data.src_mesh], g_textrans_data.dst_size[1],
        g_textrans_data.dst_size[0], g_textrans_data.output);

    ugu::Image1b inpaint_mask;
    ugu::Not(g_textrans_data.output.dst_mask, &inpaint_mask);
    ugu::Image3b dst_tex_vis;
    ugu::ConvertTo(g_textrans_data.output.dst_tex, &dst_tex_vis);
    ugu::Image3b dst_tex_vis_inpainted = dst_tex_vis.clone();
    ugu::Inpaint(inpaint_mask, dst_tex_vis_inpainted, 3.f);

    auto mats = g_textrans_data.src_mesh->materials();
    mats[0].diffuse_tex = dst_tex_vis_inpainted;
    mats[0].diffuse_texname = "transferred.png";
    mats[0].diffuse_texpath = "transferred.png";
    g_textrans_data.src_mesh->set_materials(mats);

    timer.End();
    g_callback_message = "Texture transfer took " +
                         std::to_string(timer.elapsed_msec() / 1000) + " sec.";

    std::cout << g_callback_message << std::endl;

    g_callback_finished = true;
    g_textrans_run = AlgorithmStatus::HALTING;
  }
}

void AlgorithmProcess() {
  while (!g_algorithm_process_finish) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    IcpProcess();

    NonrigidIcpProcess();

    TextransProcess();
  }
}

void Draw(GLFWwindow *window);

auto GetWidthHeightForView() {
  return std::make_pair(static_cast<uint32_t>(g_width / g_views.size()),
                        static_cast<uint32_t>(g_height));
}

bool IsCursorOnView(uint32_t vidx) {
  uint32_t x = static_cast<uint32_t>(g_cursor_pos.x());
  uint32_t unit_w = g_width / static_cast<uint32_t>(g_views.size());

  if (unit_w * vidx <= x && x < unit_w * (vidx + 1)) {
    return true;
  }

  return false;
}

template <typename T>
int CalcFillDigits(const T &point_num) {
  return std::max(
      2, static_cast<int>(std::log10(static_cast<double>(point_num)) + 1));
}

struct SplitViewInfo {
  RendererGlPtr renderer;
  PinholeCameraPtr camera;
  Eigen::Vector2i offset = {0, 0};
  std::unordered_map<RenderableMeshPtr, int> selected_point_idx;
  uint32_t id = ~0u;
  double trans_speed = 0.0;
  double wheel_speed = 0.0;
  double rotate_speed = 0.0;
  Eigen::Translation3d offset_to_rot_center = {0.0, 0.0, 0.0};

  void Init(uint32_t vidx) {
    std::lock_guard<std::mutex> lock(views_mtx);

    auto [w, h] = GetWidthHeightForView();

    id = vidx;

    offset.x() = vidx * w;
    offset.y() = 0;

    camera = std::make_shared<PinholeCamera>(w, h, 45.f);
    renderer = std::make_shared<RendererGl>();
    renderer->SetSize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    renderer->SetCamera(camera);
    renderer->Init();

    renderer->SetBackgroundColor(default_clear_color);
    renderer->SetWireColor(default_wire_color);

    renderer->SetShowWire(false);
    renderer->SetFlatNormal(true);
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(views_mtx);

    auto [w, h] = GetWidthHeightForView();

    camera->set_size(w, h);
    camera->set_fov_y(45.0f);
    camera->set_principal_point({w / 2.f, h / 2.f});
    camera->set_c2w(Eigen::Affine3d::Identity());

    renderer->SetSize(w, h);

    offset.x() = id * w;
    offset.y() = 0;

    ResetGl();
  }

  void ResetGl() {
    std::unordered_map<RenderableMeshPtr, bool> visibility;
    for (const auto &mesh : g_meshes) {
      visibility[mesh] = renderer->GetVisibility(mesh);
    }

    renderer->ClearGlState();
    for (const auto &mesh : g_meshes) {
      renderer->SetMesh(mesh, g_model_matrices.at(mesh), g_update_bvh.at(mesh));
      renderer->AddSelectedPositions(mesh,
                                     ExtractPos(g_selected_positions.at(mesh)));
      renderer->SetVisibility(mesh, visibility[mesh]);
    }

    renderer->Init();
  }

  void SetDefaultDragSpeed() {
    Eigen::Vector3f bb_max, bb_min;
    renderer->GetMergedBoundingBox(bb_max, bb_min);

    rotate_speed = ugu::pi / 180 * 10;
    wheel_speed = (bb_max - bb_min).maxCoeff() / 20;
    trans_speed = (bb_max - bb_min).maxCoeff() / g_height;
  }

  void SetDefaultDragSpeed(RenderableMeshPtr target) {
    auto stats = target->GetStatsWithTransform(g_model_matrices[target]);
    Eigen::Vector3f bb_max = stats.bb_max;
    Eigen::Vector3f bb_min = stats.bb_min;

    rotate_speed = ugu::pi / 180 * 10;
    wheel_speed = (bb_max - bb_min).maxCoeff() / 20;
    trans_speed = (bb_max - bb_min).maxCoeff() / g_height;
  }

  void SetProperCameraForTargetMesh(RenderableMeshPtr target) {
    auto stats = target->GetStatsWithTransform(g_model_matrices[target]);
    float z_trans = (stats.bb_max - stats.bb_min).maxCoeff() * 2.0f;
    float near_z = static_cast<float>(z_trans * 0.5f / 10);
    float far_z = static_cast<float>(z_trans * 2.f * 10);
    renderer->SetNearFar(near_z, far_z);

    Eigen::Vector3f view_dir =
        camera->c2w().matrix().block(0, 2, 3, 1).cast<float>();
    Eigen::Vector3f up = camera->c2w().matrix().block(0, 1, 3, 1).cast<float>();

    float max_len = (stats.bb_max - stats.bb_min).maxCoeff();
    Eigen::Vector3f pos = stats.center + max_len * 2.f * view_dir;
    Eigen::Matrix4f T;
    c2w(pos, stats.center, up, &T, true);

    Eigen::Affine3d c2w;
    c2w.matrix() = T.cast<double>();
    camera->set_c2w(c2w);

    SetDefaultDragSpeed(target);
  }

  CastRayResult CastRay() {
    // Cast ray
    Eigen::Vector3f dir_c_cv;
    // Substract offset to align cursor pos on window to corresponding positon
    // on framebuffer
    camera->ray_c(static_cast<float>(g_cursor_pos[0] - offset.x()),
                  static_cast<float>(g_cursor_pos[1] - offset.y()), &dir_c_cv);

    const Eigen::Affine3d ray_offset =
        Eigen::Affine3d(Eigen::AngleAxisd(pi, Eigen::Vector3d::UnitX()))
            .inverse();
    Eigen::Vector3f dir_c_gl =
        (dir_c_cv.transpose() * ray_offset.rotation().cast<float>());
    Eigen::Vector3f dir_w_gl =
        camera->c2w().rotation().cast<float>() * dir_c_gl;

    size_t min_geoid = ~0u;
    // float min_geo_dist = std::numeric_limits<float>::max();
    IntersectResult min_intersect;
    min_intersect.t = std::numeric_limits<float>::max();
    Eigen::Vector3f min_geo_dist_pos =
        Eigen::Vector3f::Constant(std::numeric_limits<float>::max());

    Ray ray;
    ray.dir = dir_w_gl;
    ray.org = camera->c2w().translation().cast<float>();
    auto results_all = renderer->Intersect(ray);

    for (size_t geoid = 0; geoid < g_meshes.size(); geoid++) {
      if (!renderer->GetVisibility(g_meshes[geoid])) {
        continue;
      }
      const std::vector<IntersectResult> &results = results_all[geoid];
      if (!results.empty()) {
        if (results[0].t < min_intersect.t) {
          min_geoid = geoid;
          min_intersect = results[0];
          min_geo_dist_pos = results[0].t * ray.dir + ray.org;
        }
      }
    }

    if (min_geoid != ~0u) {
      // std::cout << "closest geo: " << min_geoid << std::endl;
    }

    CastRayResult result;
    result.min_geoid = min_geoid;
    result.intersection = min_intersect;
    // result.min_geo_dist_pos = min_geo_dist_pos;

    return result;
  }

  auto FindClosestSelectedPoint(const Eigen::Vector2d &cursor_pos) {
    bool not_close = true;
    size_t closest_selected_id = ~0u;
    double min_dist = std::numeric_limits<double>::max();
    uint32_t min_geoid = ~0u;
    // RenderableMeshPtr closest_mesh = nullptr;
    float near_z, far_z;
    renderer->GetNearFar(near_z, far_z);
    Eigen::Matrix4f view_mat = camera->c2w().inverse().matrix().cast<float>();
    Eigen::Matrix4f prj_mat = camera->ProjectionMatrixOpenGl(near_z, far_z);
    for (size_t k = 0; k < g_meshes.size(); k++) {
      const auto &mesh = g_meshes[k];
      if (g_selected_positions.find(mesh) == g_selected_positions.end()) {
        continue;
      }
      if (!renderer->GetVisibility(mesh)) {
        continue;
      }
      for (size_t i = 0; i < g_selected_positions[mesh].size(); i++) {
        const auto &p_wld = GetPos(g_selected_positions[mesh][i]);
        auto [front_id, results_all] = renderer->TestVisibility(p_wld);
        if (front_id == ~0u) {
          continue;
        }

        if (renderer->GetMeshId(mesh) != front_id) {
          continue;
        }

        Eigen::Vector4f p_cam =
            view_mat * Eigen::Vector4f(p_wld.x(), p_wld.y(), p_wld.z(), 1.f);
        Eigen::Vector4f p_ndc = prj_mat * p_cam;
        p_ndc /= p_ndc.w();  // NDC [-1:1]

        // [-1:1],[-1:1] -> [0:w], [0:h]
        Eigen::Vector2d p_gl_frag =
            Eigen::Vector2d(((p_ndc.x() + 1.f) / 2.f) * camera->width(),
                            ((p_ndc.y() + 1.f) / 2.f) * camera->height());

        p_gl_frag.y() = camera->height() - p_gl_frag.y();

        // Add offset
        p_gl_frag += offset.cast<double>();

        double dist = (p_gl_frag - cursor_pos).norm();
        if (dist < g_drag_point_pix_dist_th && dist < min_dist) {
          not_close = false;
          min_dist = dist;
          closest_selected_id = i;
          min_geoid = static_cast<uint32_t>(k);
        }
      }
    }

    return std::make_tuple(!not_close, min_geoid, closest_selected_id,
                           min_dist);
  }
};

static void glfw_error_callback(int error, const char *description) {
  fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

void Clear() {
  g_meshes.clear();
  g_mesh_names.clear();
  g_mesh_paths.clear();
  g_selected_positions.clear();
  for (auto &view : g_views) {
    view.ResetGl();
  }
}

void key_callback(GLFWwindow *pwin, int key, int scancode, int action,
                  int mods) {
  (void)pwin, (void)scancode, (void)mods;
  if (key == GLFW_KEY_UP && action == GLFW_PRESS) {
    // printf("key up\n");
  }
  if (key == GLFW_KEY_DOWN && action == GLFW_PRESS) {
    // printf("key down\n");
  }
  if (key == GLFW_KEY_LEFT && action == GLFW_PRESS) {
    // printf("key left\n");
  }
  if (key == GLFW_KEY_RIGHT && action == GLFW_PRESS) {
    // printf("key right\n");
  }

  if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
    if (action == GLFW_PRESS) {
      // const char *key_name = glfwGetKeyName(key, 0);
      // printf("key - %s\n", key_name);
    }
  }
  if (key == GLFW_KEY_R) {
    if (action == GLFW_PRESS) {
      if (!ImGui::GetIO().WantCaptureKeyboard) {
        Clear();
      }
    }
  }
}

void mouse_button_callback(GLFWwindow *pwin, int button, int action, int mods) {
  (void)pwin, (void)mods;

  if (button == GLFW_MOUSE_BUTTON_LEFT) {
    g_mouse_l_pressed = action == GLFW_PRESS;
    if (g_mouse_l_pressed) {
      g_mouse_l_pressed_pos = g_cursor_pos;
    } else {
      g_mouse_l_released_pos = g_cursor_pos;

      if ((g_mouse_l_pressed_pos - g_mouse_l_released_pos).norm() > drag_th) {
      }
    }
  }

  if (button == GLFW_MOUSE_BUTTON_RIGHT) {
    g_mouse_r_pressed = action == GLFW_PRESS;
    if (g_mouse_r_pressed) {
      g_mouse_r_pressed_pos = g_cursor_pos;
    } else {
      g_mouse_r_released_pos = g_cursor_pos;
    }

    if (g_mouse_r_pressed) {
      bool not_close = false;
      CastRayResult result;
      // uint32_t min_geoid;
      // double min_dist = std::numeric_limits<double>::max();
      auto &this_view = g_views[g_subwindow_id];
      result = this_view.CastRay();
      if (result.min_geoid != ~0u &&
          this_view.renderer->GetVisibility(g_meshes[result.min_geoid])) {
        auto [is_close, min_geoid_, id, min_dist_] =
            this_view.FindClosestSelectedPoint(g_mouse_r_pressed_pos);
        not_close = !is_close;

        // min_dist = min_dist_;
        // min_geoid = min_geoid_;
      }

      if (not_close) {
        g_selected_positions[g_meshes[result.min_geoid]].push_back(result);
        for (size_t vidx = 0; vidx < g_views.size(); vidx++) {
          auto &view = g_views[vidx];
          view.renderer->AddSelectedPositions(
              g_meshes[result.min_geoid],
              ExtractPos(g_selected_positions[g_meshes[result.min_geoid]]));
        }

        // std::cout << "added " << min_dist << std::endl;
      } else {
        // std::cout << "ignored " << min_dist << std::endl;
      }
    }
  }

  if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
    g_mouse_m_pressed = action == GLFW_PRESS;
    if (g_mouse_m_pressed) {
      g_mouse_m_pressed_pos = g_cursor_pos;
    } else {
      g_mouse_m_released_pos = g_cursor_pos;

      if ((g_mouse_m_pressed_pos - g_mouse_m_released_pos).norm() > drag_th) {
      }
    }
  }
}

void mouse_wheel_callback(GLFWwindow *window, double xoffset, double yoffset) {
  (void)window, (void)xoffset;

  g_mouse_wheel_yoffset = yoffset;
  g_to_process_wheel = true;
}

void cursor_pos_callback(GLFWwindow *window, double xoffset, double yoffset) {
  (void)window;
  g_prev_cursor_pos = g_cursor_pos;

  g_cursor_pos[0] = xoffset;
  g_cursor_pos[1] = yoffset;
  if (g_mouse_l_pressed) {
    g_to_process_drag_l = true;
  }
  if (g_mouse_m_pressed) {
    g_to_process_drag_m = true;
  }
  if (g_mouse_r_pressed) {
    g_to_process_drag_r = true;
  }

  g_prev_subwindow_id = g_subwindow_id;
  for (uint32_t vidx = 0; vidx < static_cast<uint32_t>(g_views.size());
       vidx++) {
    if (IsCursorOnView(vidx)) {
      g_subwindow_id = vidx;
      break;
    }
  }
}

void LoadMesh(const std::string &path) {
  if (4 <= g_meshes.size()) {
    LOGE("#max_geom is 4\n");
    return;
  }

  auto ext = ugu::ExtractExt(path);
  auto mesh = ugu::RenderableMesh::Create();
  if (ext == "obj" || ext == "OBJ") {
    std::string obj_path = path;
    std::string obj_dir = ExtractDir(obj_path);
    if (!mesh->LoadObj(obj_path, obj_dir)) {
      return;
    }

    auto mat = mesh->materials();
    if (mat[0].diffuse_tex.empty()) {
      mat[0].diffuse_tex = Image3b(1, 1);
      auto &col = mat[0].diffuse_tex.at<Vec3b>(0, 0);

      static uint32_t count = 0;
      static Vec3b color_table[256] = {
          {125, 125, 200}, {245, 156, 62}, {118, 184, 0}, {32, 33, 36}};
      if (count == 0) {
        size_t seed = 0;
        std::uniform_int_distribution<int> dist(0, 255);
        std::default_random_engine engine(static_cast<unsigned int>(seed));
        for (int i = 4; i < 256; i++) {
          color_table[i][0] = static_cast<uint8_t>(dist(engine));
          color_table[i][1] = static_cast<uint8_t>(dist(engine));
          color_table[i][2] = static_cast<uint8_t>(dist(engine));
        }
      }
      col = color_table[count % 256];
      count++;

      mat[0].diffuse_texname = "tmp.png";
      mat[0].diffuse_texpath = "tmp.png";
      mesh->set_materials(mat);
    }

    g_mesh_names.push_back(ugu::ExtractFilename(obj_path, true));
    g_mesh_paths.push_back(obj_path);
    g_meshes.push_back(mesh);
    g_model_matrices[mesh] = Eigen::Affine3f::Identity();
    g_update_bvh[mesh] = true;
    g_selected_positions[mesh] = {};
  } else {
    LOGE("Supported extensiton: .obj\n");
    return;
  }

  for (auto &view : g_views) {
    // Temporary set for visibility
    view.renderer->SetMesh(mesh, g_model_matrices.at(mesh), false);

    view.ResetGl();

    // Reset camera pos

    Eigen::Vector3f bb_max, bb_min;
    view.renderer->GetMergedBoundingBox(bb_max, bb_min);
    float z_trans = (bb_max - bb_min).maxCoeff() * 2.0f;
    view.renderer->SetNearFar(static_cast<float>(z_trans * 0.5f / 10),
                              static_cast<float>(z_trans * 2.f * 10));

    Eigen::Affine3d c2w = Eigen::Affine3d::Identity();
    c2w.translation() = Eigen::Vector3d(0, 0, z_trans);
    view.camera->set_c2w(c2w);

    view.SetDefaultDragSpeed();
  }
}

void drop_callback(GLFWwindow *window, int count, const char **paths) {
  (void)window;
  for (int i = 0; i < count; i++) {
    std::cout << "Dropped: " << i << "/" << count << " " << paths[i]
              << std::endl;
  }
  LoadMesh(paths[0]);
}

void window_size_callback(GLFWwindow *window, int width, int height) {
  (void)window;

  if (width < 1 && height < 1) {
    return;
  }

  g_width = width;
  g_height = height;

  for (auto &view : g_views) {
    auto org_c2w = view.camera->c2w();
    view.Reset();
    view.camera->set_c2w(org_c2w);
  }
}

void framebuffer_size_callback(GLFWwindow *window, int width, int height) {
  (void)width, (void)height;

  Draw(window);
}

void cursor_enter_callback(GLFWwindow *window, int entered) {
  (void)window, (void)entered;

  g_to_process_drag_l = false;
  g_to_process_drag_r = false;
  g_to_process_drag_m = false;

  g_subwindow_id = ~0u;
}

void SetupWindow(GLFWwindow *window) {
  if (window == NULL) return;
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);  // Enable vsync

  glfwSetCursorPosCallback(window, cursor_pos_callback);

  glfwSetKeyCallback(window, key_callback);

  glfwSetMouseButtonCallback(window, mouse_button_callback);

  glfwSetScrollCallback(window, mouse_wheel_callback);

  glfwSetDropCallback(window, drop_callback);

  glfwSetCursorEnterCallback(window, cursor_enter_callback);

  glfwSetWindowSizeCallback(window, window_size_callback);
  glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
}

void DrawViews() {
  glViewport(0, 0, g_width, g_height);

  for (size_t i = 0; i < g_views.size(); i++) {
    const auto &view = g_views[i];

    for (const auto &mesh : g_meshes) {
      view.renderer->SetMesh(mesh, g_model_matrices.at(mesh),
                             g_update_bvh.at(mesh));
    }
    const uint32_t offset_w = g_width / static_cast<uint32_t>(g_views.size());
    view.renderer->SetViewport(static_cast<uint32_t>(offset_w * i), 0, offset_w,
                               g_height);
    view.renderer->Draw();
  }

  for (const auto &mesh : g_meshes) {
    g_update_bvh[mesh] = false;
  }
}

void ProcessDrags() {
  // std::lock_guard<std::mutex> lock(mouse_mtx);

  if (!ImGui::GetIO().WantCaptureMouse) {
    if (g_subwindow_id != ~0u && g_subwindow_id == g_prev_subwindow_id) {
      uint32_t vidx = g_subwindow_id;
      auto &view = g_views[vidx];

      if (g_to_process_drag_l) {
        g_to_process_drag_l = false;
        Eigen::Vector2d diff = g_cursor_pos - g_prev_cursor_pos;
        if (diff.norm() > drag_th) {
          Eigen::Affine3d cam_pose_cur = view.camera->c2w();
          Eigen::Matrix3d R_cur = cam_pose_cur.rotation();

          Eigen::Vector3d right_axis = -R_cur.col(0);
          Eigen::Vector3d up_axis = -R_cur.col(1);

          Eigen::Quaterniond R_offset =
              Eigen::AngleAxisd(
                  2 * ugu::pi * diff[0] / g_height * view.rotate_speed,
                  up_axis) *
              Eigen::AngleAxisd(
                  2 * ugu::pi * diff[1] / g_height * view.rotate_speed,
                  right_axis);

          Eigen::Affine3d cam_pose_new = view.offset_to_rot_center * R_offset *
                                         view.offset_to_rot_center.inverse() *
                                         cam_pose_cur;

          view.camera->set_c2w(cam_pose_new);
        }

        //  g_prev_to_process_drag_l_id = vidx;
      }

      if (g_to_process_drag_m) {
        g_to_process_drag_m = false;
        Eigen::Vector2d diff = g_cursor_pos - g_prev_cursor_pos;
        if (diff.norm() > drag_th) {
          Eigen::Affine3d cam_pose_cur = view.camera->c2w();
          Eigen::Matrix3d R_cur = cam_pose_cur.rotation();

          Eigen::Vector3d right_axis = -R_cur.col(0);
          Eigen::Vector3d up_axis = R_cur.col(1);

          Eigen::Vector3d t_offset = right_axis * diff[0] * view.trans_speed +
                                     up_axis * diff[1] * view.trans_speed;

          Eigen::Affine3d cam_pose_new =
              Eigen::Translation3d(t_offset + cam_pose_cur.translation()) *
              cam_pose_cur.rotation();
          view.camera->set_c2w(cam_pose_new);
        }
      }

      if (g_to_process_drag_r) {
        g_to_process_drag_r = false;
        CastRayResult result;
        bool is_close = false;
        double min_dist;
        uint32_t min_geoid;
        size_t id;
        result = view.CastRay();
        if (result.min_geoid != ~0u) {
          std::tie(is_close, min_geoid, id, min_dist) =
              view.FindClosestSelectedPoint(g_cursor_pos);

          if (result.min_geoid == min_geoid && is_close) {
            g_selected_positions[g_meshes[min_geoid]][id] = result;
            for (size_t vidx_ = 0; vidx_ < g_views.size(); vidx_++) {
              auto &view_ = g_views[vidx_];
              view_.renderer->AddSelectedPositions(
                  g_meshes[min_geoid],
                  ExtractPos(g_selected_positions[g_meshes[min_geoid]]));
            }
          } else {
            // std::cout << "Failed " << min_dist << std::endl;
          }
        }
      }
      if (g_to_process_wheel) {
        g_to_process_wheel = false;

        Eigen::Affine3d cam_pose_cur = view.camera->c2w();
        Eigen::Vector3d t_offset = cam_pose_cur.rotation().col(2) *
                                   -g_mouse_wheel_yoffset * view.wheel_speed;
        Eigen::Affine3d cam_pose_new =
            Eigen::Translation3d(t_offset + cam_pose_cur.translation()) *
            cam_pose_cur.rotation();
        view.camera->set_c2w(cam_pose_new);
      }
    }
  }

  // Get visibile selected points
  {
    for (uint32_t vidx = 0; vidx < static_cast<uint32_t>(g_views.size());
         vidx++) {
      auto &view = g_views[vidx];
      const auto &camera = view.renderer->GetCamera();
      Eigen::Matrix4f view_mat = camera->c2w().inverse().matrix().cast<float>();
      float near_z, far_z;
      view.renderer->GetNearFar(near_z, far_z);
      Eigen::Matrix4f prj_mat = camera->ProjectionMatrixOpenGl(near_z, far_z);

      std::vector<TextRendererGl::Text> texts;
      for (const auto &geo : g_meshes) {
        if (!view.renderer->GetVisibility(geo)) {
          continue;
        }

        for (size_t i = 0; i < g_selected_positions[geo].size(); i++) {
          const auto &p = GetPos(g_selected_positions[geo][i]);

          // p is back of the camera in GL coord (+Z)
          auto cam_p = view.renderer->GetCamera()->w2c().cast<float>() * p;
          if (cam_p.z() > 0.f) {
            continue;
          }

          // Cast ray for visibility
          auto [front_id, results] = view.renderer->TestVisibility(p);

          // No hit
          if (front_id == ~0u) {
            continue;
          }

          // Other geometries
          if (view.renderer->GetMeshId(geo) != front_id) {
            continue;
          }

          // Hit the target geomtery but on the another surface
          float dist = (GetPos(results[view.renderer->GetMeshId(geo)][0],
                               view.renderer->GetMeshId(geo)) -
                        p)
                           .norm();
          if (dist > view.renderer->GetDepthThreshold()) {
            continue;
          }

          Eigen::Vector4f p_cam =
              view_mat * Eigen::Vector4f(p.x(), p.y(), p.z(), 1.f);
          Eigen::Vector4f p_ndc = prj_mat * p_cam;
          p_ndc /= p_ndc.w();  // NDC [-1:1]

          // [-1:1],[-1:1] -> [0:w], [0:h]
          TextRendererGl::Text text;
          text.body = std::to_string(i);
          text.x = ((p_ndc.x() + 1.f) / 2.f) * camera->width();
          text.y = ((p_ndc.y() + 1.f) / 2.f) * camera->height();
          text.y = camera->height() - text.y;
          text.scale = 1.f;
          text.color = Eigen::Vector3f(0.f, 0.f, 0.f);
          texts.push_back(text);
        }
      }
      view.renderer->SetTexts(texts);
    }
  }
}

void DrawImguiGeneralWindow(bool &reset_points) {
  ImGui::SetNextWindowPos({0.f, 0.f}, ImGuiCond_Once);
  ImGui::SetNextWindowCollapsed(false, ImGuiCond_Once);

  ImGui::Begin("General");

  static char mesh_path[1024] = "";
  ImGui::InputText("Mesh path", mesh_path, 1024u);
  if (ImGui::Button("Load mesh")) {
    LoadMesh(mesh_path);
  }

  static int src_id = -1;
  static int dst_id = -1;
  if (ImGui::BeginListBox("source", {50, 50})) {
    if (g_meshes.empty()) {
      src_id = -1;
    }
    for (int n = 0; n < static_cast<int>(g_meshes.size()); ++n) {
      const bool is_selected = (src_id == n);
      if (ImGui::Selectable(std::to_string(n).c_str(), is_selected)) {
        src_id = n;
      }
    }
    ImGui::EndListBox();
  }

  if (ImGui::BeginListBox("target", {50, 50})) {
    if (g_meshes.empty()) {
      dst_id = -1;
    }
    for (int n = 0; n < static_cast<int>(g_meshes.size()); ++n) {
      const bool is_selected = (dst_id == n);
      if (ImGui::Selectable(std::to_string(n).c_str(), is_selected)) {
        dst_id = n;
      }
    }
    ImGui::EndListBox();
  }

  auto validate_func = [&]() {
    if (src_id < 0 || dst_id < 0) {
      ImGui::OpenPopup("Error");
      g_error_message = "Select source and target";
      return false;
    }
    if (src_id == dst_id) {
      ImGui::OpenPopup("Error");
      g_error_message = "Source and target must be different";
      return false;
    }
    return true;
  };

  RenderableMeshPtr src_mesh = 0 <= src_id ? g_meshes[src_id] : nullptr;
  RenderableMeshPtr dst_mesh = 0 <= dst_id ? g_meshes[dst_id] : nullptr;

  static bool with_scale = false;
  ImGui::Text("Alignment by Selected Points");
  ImGui::SameLine();
  if (ImGui::Button("Run####Alignment by Selected Points")) {
    if (validate_func()) {
      auto src_points = ExtractPos(g_selected_positions[src_mesh]);
      auto dst_points = ExtractPos(g_selected_positions[dst_mesh]);

      if (3 <= src_points.size() && src_points.size() == dst_points.size()) {
        Eigen::Affine3d src2dst;

        if (with_scale) {
          src2dst = FindSimilarityTransformFrom3dCorrespondences(src_points,
                                                                 dst_points);
        } else {
          src2dst =
              FindRigidTransformFrom3dCorrespondences(src_points, dst_points);
        }
        g_model_matrices[src_mesh] =
            src2dst.cast<float>() * g_model_matrices[src_mesh];
        g_update_bvh[src_mesh] = true;

        reset_points = true;

      } else {
        ImGui::OpenPopup("Error");
        if (src_points.size() < 3) {
          g_error_message = "At least 3 correspondences";
        } else {
          g_error_message = "Must have the same number of selected points";
        }
      }
    }
  }
  if (ImGui::TreeNodeEx("Option####OptionAlignment by Selected Points")) {
    if (ImGui::Checkbox("With scale", &with_scale)) {
    }
    ImGui::TreePop();
  }

  ImGui::Text("Rigid ICP");
  ImGui::SameLine();
  if (ImGui::Button("Run####Rigid ICP")) {
    if (validate_func()) {
      auto apply_trans = [&](const std::vector<Eigen::Vector3f> &points,
                             const Eigen::Affine3f &T, bool is_normal = false) {
        std::vector<Eigen::Vector3f> transed;
        Eigen::Affine3f T_ = T;
        if (is_normal) {
          // Remove translation
          T_.matrix().block(0, 3, 3, 1).setConstant(0.f);
        }
        for (const auto &p : points) {
          Eigen::Vector3f t = T_ * p;
          if (is_normal) {
            t.normalize();
          }
          transed.push_back(t);
        }
        return transed;
      };

      auto transed_src_points =
          apply_trans(src_mesh->vertices(), g_model_matrices[src_mesh]);
      auto transed_dst_points =
          apply_trans(dst_mesh->vertices(), g_model_matrices[dst_mesh]);

      auto transed_src_normals =
          apply_trans(src_mesh->normals(), g_model_matrices[src_mesh], true);
      auto transed_dst_normals =
          apply_trans(dst_mesh->normals(), g_model_matrices[dst_mesh], true);

      ImGui::OpenPopup("Algorithm Callback");
      std::lock_guard<std::mutex> lock(icp_mtx);
      g_icp_data.src_mesh = src_mesh;
      g_icp_data.src_points = std::move(transed_src_points);
      g_icp_data.dst_points = std::move(transed_dst_points);

      g_icp_data.src_normals = std::move(transed_src_normals);
      g_icp_data.dst_normals = std::move(transed_dst_normals);

      g_icp_data.dst_faces = dst_mesh->vertex_indices();
      g_icp_data.callback = IcpProcessCallback;
      g_icp_data.output.loss_histroty.clear();
      g_icp_data.output.transform_histry.clear();

      g_callback_finished = false;
      g_icp_run = AlgorithmStatus::STARTED;
    }
  }
  if (ImGui::TreeNodeEx("Option####OptionRigid ICP")) {
    static int corresp_mode = 1;
    ImGui::Text("Correspondence");
    ImGui::RadioButton("Point(Vertex)-to-Surface(Triangle)", &corresp_mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Point(Vertex)-to-Point(Vertex)", &corresp_mode, 0);
    g_icp_data.corresp_type = static_cast<IcpCorrespType>(corresp_mode);

    static int loss_mode = 1;
    ImGui::Text("Loss");
    ImGui::RadioButton("Point-to-Plane", &loss_mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Point-to-Point", &loss_mode, 0);
    g_icp_data.loss_type = static_cast<IcpLossType>(loss_mode);

    if (ImGui::InputInt("max iter###rigid_icp_max_iter",
                        &g_icp_data.terminate_criteria.iter_max)) {
    }
    if (ImGui::InputDouble("min loss###rigid_icp_min_loss",
                           &g_icp_data.terminate_criteria.loss_min)) {
    }
    if (ImGui::InputDouble("min eps###rigid_icp_min_eps",
                           &g_icp_data.terminate_criteria.loss_eps)) {
    }

    if (ImGui::InputFloat("normal threshold (rad) ###rigid_icp_normal_th",
                          &g_icp_data.corresp_criteria.normal_th)) {
    }
    if (ImGui::InputFloat("distance threshold ###rigid_icp_dist_th",
                          &g_icp_data.corresp_criteria.dist_th)) {
    }
    if (ImGui::Checkbox(
            "Always try to use the nearest point###rigid_icp_test_nearest",
            &g_icp_data.corresp_criteria.test_nearest)) {
    }

    ImGui::TreePop();
  }

  ImGui::Text("Nonrigid ICP");
  ImGui::SameLine();
  if (ImGui::Button("Run####Nonrigid ICP")) {
    if (validate_func()) {
      ImGui::OpenPopup("Algorithm Callback");
      std::lock_guard<std::mutex> lock(nonrigidicp_mtx);
      g_nonrigidicp_data.src_mesh = src_mesh;
      g_nonrigidicp_data.dst_mesh = dst_mesh;

      g_callback_finished = false;
      g_nonrigidicp_run = AlgorithmStatus::STARTED;
    }
  }
  if (ImGui::TreeNodeEx("Option####OptionNonrigid ICP")) {
    ImGui::Checkbox("check self intersection",
                    &g_nonrigidicp_data.check_self_itersection);
    ImGui::InputFloat("angle threshold (rad)",
                      &g_nonrigidicp_data.angle_rad_th);
    ImGui::InputFloat("distance threshold", &g_nonrigidicp_data.dist_th);
    if (ImGui::InputInt("#NearestNeighbors for correspondence",
                        &g_nonrigidicp_data.nn_num)) {
      if (g_nonrigidicp_data.nn_num < 1) {
        g_nonrigidicp_data.nn_num = 1;
      }
    }

    ImGui::Checkbox("check dst geometry border",
                    &g_nonrigidicp_data.dst_check_geometry_border);
    ImGui::Checkbox("check src geometry border",
                    &g_nonrigidicp_data.src_check_geometry_border);

    ImGui::InputDouble("max stiffness", &g_nonrigidicp_data.max_alpha);
    ImGui::InputDouble("min stiffness", &g_nonrigidicp_data.min_alpha);
    ImGui::InputDouble("stiffness factor", &g_nonrigidicp_data.gamma);
    ImGui::InputDouble("landmark weight", &g_nonrigidicp_data.beta);

    ImGui::InputInt("steps###nonrigid_icp_step", &g_nonrigidicp_data.step);

    ImGui::InputInt("max iter per stiffness",
                    &g_nonrigidicp_data.max_internal_iter);
    ImGui::InputDouble("eps for params per stiffness",
                       &g_nonrigidicp_data.min_frobenius_norm_diff);

    ImGui::TreePop();
  }

  ImGui::Text("Texture transfer");
  ImGui::SameLine();
  if (ImGui::Button("Run####Texture transfer")) {
    if (validate_func()) {
      ImGui::OpenPopup("Algorithm Callback");
      std::lock_guard<std::mutex> lock(textrans_mtx);

      g_textrans_data.src_mesh = src_mesh;
      g_textrans_data.dst_mesh = dst_mesh;

      g_callback_finished = false;
      g_textrans_run = AlgorithmStatus::STARTED;
    }
  }
  if (ImGui::TreeNodeEx("Option####OptionTexture Transfer")) {
    if (ImGui::InputInt2("Texture size", g_textrans_data.dst_size.data())) {
      g_textrans_data.dst_size[0] =
          std::clamp(g_textrans_data.dst_size[0], 1, 16000);
      g_textrans_data.dst_size[1] =
          std::clamp(g_textrans_data.dst_size[1], 1, 16000);
    }
  }

  if (g_nonrigidicp_run == AlgorithmStatus::RUNNING) {
    std::lock_guard<std::mutex> lock_update(nonrigidicp_update_mtx);
    // OpenGL API must be called in the main thread
    g_nonrigidicp_data.src_mesh->UpdateMesh();
  }

  ImGui::SetNextWindowSize({200.f, 300.f}, ImGuiCond_Once);
  if (ImGui::BeginPopupModal("Algorithm Callback")) {
    // Draw popup contents.
    ImGui::Text(g_callback_message.c_str());

    if (g_callback_finished) {
      if (ImGui::Button("OK")) {
        g_callback_finished = true;
        g_callback_message = "";
        reset_points = true;
        ImGui::CloseCurrentPopup();
      }
    }

    ImGui::EndPopup();
  }

  if (ImGui::BeginPopupModal("Error")) {
    // Draw popup contents.
    ImGui::Text(g_error_message.c_str());

    if (ImGui::Button("OK")) {
      ImGui::CloseCurrentPopup();
      g_error_message = "";
    }

    ImGui::EndPopup();
  }

  ImGui::End();
}

void DrawImguiMeshes(SplitViewInfo &view, bool &reset_points) {
  const auto &transed_stats = view.renderer->GetTransedStats();

  for (size_t i = 0; i < g_meshes.size(); i++) {
    bool v = view.renderer->GetVisibility(g_meshes[i]);
    std::string label =
        std::to_string(i) + " " + g_mesh_names[i] + ": " + g_mesh_paths[i];
    if (ImGui::Checkbox(label.c_str(), &v)) {
      view.renderer->SetVisibility(g_meshes[i], v);
    }
    Eigen::Vector3f pos_col =
        view.renderer->GetSelectedPositionColor(g_meshes[i]);
    if (ImGui::ColorEdit3((label + "select color").c_str(), pos_col.data(),
                          ImGuiColorEditFlags_NoLabel)) {
      view.renderer->AddSelectedPositionColor(g_meshes[i], pos_col);
    }

    if (ImGui::Button(("Focus###focus" + std::to_string(i)).c_str())) {
      view.SetProperCameraForTargetMesh(g_meshes[i]);
    }

    const auto &stat = transed_stats.at(g_meshes[i]);
    ImGui::Text("Bounding Box Max: (%f, %f, %f)", stat.bb_max.x(),
                stat.bb_max.y(), stat.bb_max.z());
    ImGui::Text("Bounding Box Min: (%f, %f, %f)", stat.bb_min.x(),
                stat.bb_min.y(), stat.bb_min.z());
    ImGui::Text("Object Center   : (%f, %f, %f)", stat.center.x(),
                stat.center.y(), stat.center.z());

    if (ImGui::Button(
            ("Set center as rotation center###rot_center" + std::to_string(i))
                .c_str())) {
      view.offset_to_rot_center.x() = static_cast<double>(stat.center.x());
      view.offset_to_rot_center.y() = static_cast<double>(stat.center.y());
      view.offset_to_rot_center.z() = static_cast<double>(stat.center.z());
    }

    if (ImGui::Button(
            ("Move center to origin###move_center" + std::to_string(i))
                .c_str())) {
      Eigen::Affine3f model_mat = g_model_matrices.at(g_meshes[i]);
      g_model_matrices[g_meshes[i]] =
          Eigen::Translation3f(-stat.center) * model_mat;

      g_update_bvh[g_meshes[i]] = true;
      reset_points = true;
    }

    {
      auto &model_mat = g_model_matrices.at(g_meshes[i]);
      Eigen::Vector3f t, s;
      Eigen::Matrix3f R;
      DecomposeRts(model_mat, R, t, s);
      // bool update_model_mat = false;
      bool update_rts = false;
      if (ImGui::InputFloat3(("Translation###t" + std::to_string(i)).c_str(),
                             t.data())) {
        update_rts = true;
      }
      if (ImGui::InputFloat3(("Rotation###r" + std::to_string(i)).c_str(),
                             R.data()) ||
          ImGui::InputFloat3("        ", R.data() + 3) ||
          ImGui::InputFloat3("        ", R.data() + 6)) {
        update_rts = true;
      }
      if (ImGui::InputFloat3(("Scale###s" + std::to_string(i)).c_str(),
                             s.data())) {
        update_rts = true;
      }
      Eigen::Matrix4f model_mat_t =
          model_mat.matrix().transpose();  // To row-major
      if (ImGui::InputFloat4(("Affine Matrix###m" + std::to_string(i)).c_str(),
                             model_mat_t.data()) ||
          ImGui::InputFloat4("        ", model_mat_t.data() + 4) ||
          ImGui::InputFloat4("        ", model_mat_t.data() + 8) ||
          ImGui::InputFloat4("        ", model_mat_t.data() + 12)) {
        // update_model_mat = true;
      }

      if (update_rts) {
        model_mat = Eigen::Translation3f(t) * R * Eigen::Scaling(s);
        g_update_bvh[g_meshes[i]] = true;
        reset_points = true;
      }
    }

    if (ImGui::Button(("Apply transform###apply_transform" + std::to_string(i))
                          .c_str())) {
      g_meshes[i]->Transform(g_model_matrices[g_meshes[i]]);
      g_model_matrices[g_meshes[i]] = Eigen::Affine3f::Identity();
      g_update_bvh[g_meshes[i]] = true;
      reset_points = true;
    }

    static char mesh_export_path_buf[1024] = "./mesh.obj";
    ImGui::InputText(
        (std::string("Mesh Export Path###mesh_export_path") + std::to_string(i))
            .c_str(),
        mesh_export_path_buf, 1024);
    static bool apply_transform = true;
    if (ImGui::Checkbox("apply transform", &apply_transform)) {
    }
    if (ImGui::Button((std::string("Export###mesh_export") + std::to_string(i))
                          .c_str())) {
      auto save_mesh = Mesh::Create(*g_meshes[i].get());
      if (apply_transform) {
        save_mesh->Transform(g_model_matrices[g_meshes[i]]);
      }
      save_mesh->WriteObj(std::string(mesh_export_path_buf));
    }

    // ImGui::BeginListBox("Points (fid, u, v) (x, y, z)");
    // ImGui::EndListBox();
    const auto draw_list_size = ImVec2(360, 240);
    // const char *items[RendererGl::MAX_SELECTED_POS];
    if (view.selected_point_idx.find(g_meshes[i]) ==
        view.selected_point_idx.end()) {
      view.selected_point_idx[g_meshes[i]] = 0;
    }
    auto &points = g_selected_positions[g_meshes[i]];

    if (view.selected_point_idx[g_meshes[i]] >=
        static_cast<int>(points.size())) {
      view.selected_point_idx[g_meshes[i]] = 0;
    }

    std::vector<std::string> lines;  // For owership of const char*

    int fill_digits = CalcFillDigits(points.size());
    for (size_t pidx = 0; pidx < points.size(); pidx++) {
      auto p_wld = GetPos(points[pidx]);
      std::string line = ugu::zfill(pidx, fill_digits) + ": (" +
                         std::to_string(points[pidx].intersection.fid) + ", " +
                         std::to_string(points[pidx].intersection.u) + ", " +
                         std::to_string(points[pidx].intersection.v) + ")" +
                         " (" + std::to_string(p_wld[0]) + ", " +
                         std::to_string(p_wld[1]) + ", " +
                         std::to_string(p_wld[2]) + ")";
      lines.push_back(line);
    }
    if (ImGui::BeginListBox((std::to_string(i) + " : " +
                             std::string("Points (fid, u, v) (x, y, z)"))
                                .c_str(),
                            draw_list_size)) {
      for (int n = 0; n < static_cast<int>(points.size()); ++n) {
        const bool is_selected = (view.selected_point_idx[g_meshes[i]] == n);
        if (ImGui::Selectable(lines[n].c_str(), is_selected)) {
          view.selected_point_idx[g_meshes[i]] = n;
        }
      }
      ImGui::EndListBox();
    }

    if (ImGui::Button((std::string("Remove###remove_point") + std::to_string(i))
                          .c_str())) {
      if (static_cast<int>(points.size()) >
          view.selected_point_idx[g_meshes[i]]) {
        points.erase(points.begin() + view.selected_point_idx[g_meshes[i]]);
      }
      reset_points = true;
    }

    static PointOnFaceType pof_type = PointOnFaceType::POINT_ON_TRIANGLE;
    if (ImGui::BeginListBox(
            (std::string("Point Type###ptype") + std::to_string(i)).c_str(),
            {200, 70})) {
      std::array<std::string, 3> names = {"Named Point on Triangle",
                                          "Point on Triangle", "3D-Point"};
      static int type_id = -1;
      for (int n = 0; n < static_cast<int>(names.size()); ++n) {
        const bool this_is_selected = (type_id == n);
        if (ImGui::Selectable(names[n].c_str(), this_is_selected)) {
          type_id = n;
        }
      }

      if (0 <= type_id) {
        pof_type = static_cast<PointOnFaceType>(type_id);
      }

      ImGui::EndListBox();
    }

    static char import_path_buf[1024] = "./points.json";
    ImGui::InputText(
        (std::string("Import Path###inport_path") + std::to_string(i)).c_str(),
        import_path_buf, 1024);

    if (ImGui::Button(
            (std::string("Import###import") + std::to_string(i)).c_str())) {
      if (pof_type == PointOnFaceType::THREED_POINT) {
        ImGui::OpenPopup("Error");
        g_error_message = "Not supported yet";
      } else {
        std::vector<PointOnFace> pofs;

        try {
          pofs = LoadPoints(std::string(import_path_buf), pof_type);
        } catch (const std::exception &) {
          std::cout << "Failed to load" << std::endl;
        }

        if (!pofs.empty()) {
          g_selected_positions[g_meshes[i]].clear();
          for (const auto &pof : pofs) {
            CastRayResult res;
            res.min_geoid = i;
            res.intersection.fid = pof.fid;
            res.intersection.u = pof.u;
            res.intersection.v = pof.v;
            g_selected_positions[g_meshes[i]].push_back(res);
          }

          reset_points = true;
        }
      }
    }

    static char export_path_buf[1024] = "./points.json";
    ImGui::InputText(
        (std::string("Points Export Path###export_path") + std::to_string(i))
            .c_str(),
        export_path_buf, 1024);

    if (ImGui::Button(
            (std::string("Export###export") + std::to_string(i)).c_str())) {
      int fill_digits_export = CalcFillDigits(points.size());
      std::vector<PointOnFace> pofs;
      for (size_t p_idx = 0; p_idx < points.size(); p_idx++) {
        const auto &p = points[p_idx];
        PointOnFace pof;
        pof.name = zfill(p_idx, fill_digits_export);
        pof.fid = p.intersection.fid;
        pof.u = p.intersection.u;
        pof.v = p.intersection.v;
        pof.pos = GetPos(p);
        pofs.push_back(pof);
      }
      WritePoints(std::string(export_path_buf), pofs, pof_type);
    }
  }
}

void DrawImguiCamera(SplitViewInfo &view) {
  bool update_pose = false;
  Eigen::Affine3f c2w_gl = view.camera->c2w().cast<float>();
  Eigen::Vector3f pos = c2w_gl.translation();
  Eigen::Matrix3f R_gl = c2w_gl.rotation();
  Eigen::Matrix3f R_cv = R_gl;
  R_cv.col(1) *= -1.f;
  R_cv.col(2) *= -1.f;

  if (ImGui::InputDouble("rotate speed", &view.rotate_speed)) {
  }
  if (ImGui::InputDouble("trans speed", &view.trans_speed)) {
  }
  if (ImGui::InputDouble("wheel speed", &view.wheel_speed)) {
  }

  float rot_center[3] = {
      static_cast<float>(view.offset_to_rot_center.x()),
      static_cast<float>(view.offset_to_rot_center.y()),
      static_cast<float>(view.offset_to_rot_center.z()),
  };
  if (ImGui::InputFloat3("Mouse rotation center", rot_center)) {
    view.offset_to_rot_center.x() = static_cast<double>(rot_center[0]);
    view.offset_to_rot_center.y() = static_cast<double>(rot_center[1]);
    view.offset_to_rot_center.z() = static_cast<double>(rot_center[2]);
  }

  Eigen::Vector2f nearfar;
  view.renderer->GetNearFar(nearfar[0], nearfar[1]);
  if (ImGui::InputFloat2("near far", nearfar.data())) {
    view.renderer->SetNearFar(nearfar[0], nearfar[1]);
  }

  Eigen::Vector2i size(view.camera->width(), view.camera->height());
  if (ImGui::InputInt2("width height", size.data())) {
    // view.camera->set_size(size[0], size[1]);
    // TODO: Window resize
  }

  Eigen::Vector2f fov(view.camera->fov_x(), view.camera->fov_y());
  Eigen::Vector2f fov_org = fov;
  if (ImGui::InputFloat2("FoV-X FoV-Y", fov.data())) {
    if (std::abs(fov_org[0] - fov[0]) > 0.01f) {
      view.camera->set_fov_x(fov[0]);
    } else {
      view.camera->set_fov_y(fov[1]);
    }
  }

  if (ImGui::TreeNodeEx("OpenCV Style", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::InputFloat3("Position", pos.data())) {
      update_pose = true;
    }

    if (ImGui::InputFloat3("Rotation", R_cv.data()) ||
        ImGui::InputFloat3("        ", R_cv.data() + 3) ||
        ImGui::InputFloat3("        ", R_cv.data() + 6)) {
      update_pose = true;
      R_gl = R_cv;
      R_gl.col(1) *= -1.f;
      R_gl.col(2) *= -1.f;
    }

    Eigen::Vector2f fxfy = view.camera->focal_length();
    if (ImGui::InputFloat2("fx fy", fxfy.data())) {
      view.camera->set_focal_length(fxfy);
    }

    Eigen::Vector2f cxcy = view.camera->principal_point();
    if (ImGui::InputFloat2("cx cy", cxcy.data())) {
      view.camera->set_principal_point(cxcy);
    }

    Eigen::Vector4f distortion(0.f, 0.f, 0.f, 0.f);
    if (ImGui::InputFloat4("k1 k2 p1 p2 (TODO)", distortion.data())) {
      // TODO
    }

    ImGui::TreePop();
  }

  if (ImGui::TreeNodeEx("OpenGL Style", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::InputFloat3("Position", pos.data())) {
      update_pose = true;
    }

    if (ImGui::InputFloat3("Rotation", R_gl.data()) ||
        ImGui::InputFloat3("        ", R_gl.data() + 3) ||
        ImGui::InputFloat3("        ", R_gl.data() + 6)) {
      update_pose = true;
    }

    Eigen::Matrix4f prj_gl_org =
        view.camera->ProjectionMatrixOpenGl(nearfar[0], nearfar[1]);
    Eigen::Matrix4f prj_gl = prj_gl_org.transpose();  // To row-major
    if (ImGui::InputFloat4("Projection", prj_gl.data()) ||
        ImGui::InputFloat4("        ", prj_gl.data() + 4) ||
        ImGui::InputFloat4("        ", prj_gl.data() + 8) ||
        ImGui::InputFloat4("        ", prj_gl.data() + 12)) {
      ugu::LOGI("No update for GL projection matrix\n");
    }

    if (update_pose) {
      c2w_gl = R_gl * Eigen::Translation3f(pos);
      view.camera->set_c2w(c2w_gl.cast<double>());
    }

    ImGui::TreePop();
  }

  bool show_wire = view.renderer->GetShowWire();
  if (ImGui::Checkbox("show wire", &show_wire)) {
    view.renderer->SetShowWire(show_wire);
  }

  bool flat_normal = view.renderer->GetFlatNormal();
  if (ImGui::Checkbox("flat normal", &flat_normal)) {
    view.renderer->SetFlatNormal(flat_normal);
  }

  Eigen::Vector3f wire_col = view.renderer->GetWireColor();
  if (ImGui::ColorEdit3("wire color", wire_col.data())) {
    view.renderer->SetWireColor(wire_col);
  }

  Eigen::Vector3f bkg_col = view.renderer->GetBackgroundColor();
  if (ImGui::ColorEdit3("background color", bkg_col.data())) {
    view.renderer->SetBackgroundColor(bkg_col);
  }

  static int save_counter = 0;
  static char gbuf_save_path[1024] = "./";
  ImGui::InputText("GBuffer Save Dir.", gbuf_save_path, 1024u);
  ImGui::Text("Prefix %d", save_counter);
  ImGui::SameLine();
  if (ImGui::Button("Save")) {
    std::string prefix = std::to_string(save_counter) + "_";

    view.renderer->ReadGbuf();
    GBuffer gbuf;
    view.renderer->GetGbuf(gbuf);

    imwrite(prefix + "pos_wld.bin", gbuf.pos_wld);
    imwrite(prefix + "pos_cam.bin", gbuf.pos_cam);
    Image3b vis_pos_wld, vis_pos_cam;
    vis_pos_wld = ColorizePosMap(gbuf.pos_wld);
    imwrite(prefix + "pos_wld.jpg", vis_pos_wld);
    vis_pos_cam = ColorizePosMap(gbuf.pos_cam);
    imwrite(prefix + "pos_cam.jpg", vis_pos_cam);

    imwrite(prefix + "normal_wld.bin", gbuf.normal_wld);
    imwrite(prefix + "normal_cam.bin", gbuf.normal_cam);
    Image3b vis_normal_wld, vis_normal_cam;
    Normal2Color(gbuf.normal_wld, &vis_normal_wld, true);
    imwrite(prefix + "normal_wld.jpg", vis_normal_wld);
    Normal2Color(gbuf.normal_cam, &vis_normal_cam, true);
    imwrite(prefix + "normal_cam.jpg", vis_normal_cam);

    imwrite(prefix + "depth01.bin", gbuf.depth_01);
    Image3b vis_depth;
    Depth2Color(gbuf.depth_01, &vis_depth, 0.f, 1.f);
    imwrite(prefix + "depth01.jpg", vis_depth);

    Image1b geoid_1b;
    gbuf.geo_id.convertTo(geoid_1b, CV_8UC1);
    imwrite(prefix + "geoid.png", geoid_1b);
    Image3b vis_geoid;
    FaceId2RandomColor(gbuf.geo_id, &vis_geoid);
    imwrite(prefix + "geoid.jpg", vis_geoid);

    imwrite(prefix + "faceid.bin", gbuf.face_id);
    Image3b vis_faceid;
    FaceId2RandomColor(gbuf.face_id, &vis_faceid);
    imwrite(prefix + "faceid.jpg", vis_faceid);

    imwrite(prefix + "bary.bin", gbuf.bary);
    Image3b vis_bary = ColorizeBarycentric(gbuf.bary);
    imwrite(prefix + "bary.jpg", vis_bary);

    imwrite(prefix + "uv.bin", gbuf.uv);
    Image3b vis_uv = ColorizeBarycentric(gbuf.uv);
    imwrite(prefix + "uv.jpg", vis_uv);

    imwrite(prefix + "color.png", gbuf.color);

    save_counter++;
  }
}

void DrawImgui(GLFWwindow *window) {
  std::lock_guard<std::mutex> lock(views_mtx);

  bool reset_points = false;
  auto [w, h] = GetWidthHeightForView();

  for (size_t j = 0; j < g_views.size(); j++) {
    auto &view = g_views[j];
    const std::string title = std::string("View ") + std::to_string(j);

    ImGui::SetNextWindowSize({static_cast<float>(w / 2), static_cast<float>(h)},
                             ImGuiCond_Once);
    ImGui::SetNextWindowPos({static_cast<float>(w * (j + 0.5)), 50.f},
                            ImGuiCond_Once);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_Once);

    ImGui::Begin(title.c_str());
    if (ImGui::TreeNodeEx("Meshes", ImGuiTreeNodeFlags_DefaultOpen)) {
      DrawImguiMeshes(view, reset_points);
      ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
      DrawImguiCamera(view);
      ImGui::TreePop();
    }

    ImGui::End();
  }

  DrawImguiGeneralWindow(reset_points);

  if (reset_points) {
    for (auto &view : g_views) {
      view.ResetGl();
    }
    for (size_t gidx = 0; gidx < g_meshes.size(); gidx++) {
      for (size_t vidx = 0; vidx < g_views.size(); vidx++) {
        auto &view = g_views[vidx];
        view.renderer->AddSelectedPositions(
            g_meshes[gidx], ExtractPos(g_selected_positions[g_meshes[gidx]]));
      }
    }
  }

  // Draw divider lines
  auto drawlist = ImGui::GetBackgroundDrawList();
  for (size_t vidx = 0; vidx < g_views.size() - 1; vidx++) {
    const float thickness = 2.f;
    float w_c = static_cast<float>((vidx + 1) * w) - thickness / 2;
    drawlist->AddLine({w_c, 0.f}, {w_c, static_cast<float>(h)},
                      ImGui::GetColorU32(IM_COL32(50, 50, 50, 255)), thickness);
  }

  ImGui::Render();
  int display_w, display_h;
  glfwGetFramebufferSize(window, &display_w, &display_h);
  glViewport(0, 0, display_w, display_h);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Draw(GLFWwindow *window) {
  glClear(GL_COLOR_BUFFER_BIT);
  // glClearColor(g_views[0].clear_color.x(), g_views[0].clear_color.y(),
  //              g_views[0].clear_color.z(), 1.f);

  // Start the Dear ImGui frame
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  DrawViews();

  ProcessDrags();
  glClear(GL_DEPTH_BUFFER_BIT);

  DrawImgui(window);

  glfwSwapBuffers(window);
}

void PrintUsage() {
  std::string usage =
      R"(########################### Devenir User Guide #################################
Load Model (.obj only)      : "Load Mesh" button or drag & drop

Camera Translation XY       : Wheel drag
Camera Translation Z (zoom) : Wheel scroll
Camera Rotation (pitch, yaw): Left drag

Point Add                   : Right click on a mesh
Point Move                  : Right drag near a point
###############################################################################)";

  std::cout << usage << std::endl;
}

}  // namespace

int main(int, char **) {
  // Setup window
  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit()) return 1;

    // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
  // GL ES 2.0 + GLSL 100
  const char *glsl_version = "#version 100";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
  // GL 3.2 + GLSL 150
  const char *glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else

#if 0
  const char *glsl_version = "#version 450";

  // Upgrade WSL's OpenGL to 4.5
  // https://zenn.dev/suudai/articles/a25e3ed0a944c7

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#else

  const char *glsl_version = "#version 330";

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#endif

#endif

  // Create window with graphics context
  GLFWwindow *window = glfwCreateWindow(
      g_width, g_height, "Devenir: An Interactive Mesh Retopology Tool", NULL,
      NULL);

  SetupWindow(window);

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable
  // Keyboard Controls io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; //
  // Enable Gamepad Controls

  const int version = gladLoadGL(glfwGetProcAddress);
  if (version == 0) {
    fprintf(stderr, "Failed to load OpenGL 3.x/4.x libraries!\n");
    return 1;
  }

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();
  // ImGui::StyleColorsLight();

  // Setup Platform/Renderer backends
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  // ImGui_ImplGlfw_InitForOpenGL(window2, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  glEnable(GL_DEPTH_TEST);

  g_views.resize(MAX_N_SPLIT_WIDTH);

  for (size_t vidx = 0; vidx < g_views.size(); vidx++) {
    auto &view = g_views[vidx];
    view.Init(static_cast<uint32_t>(vidx));
  }

  std::thread algorithm_thread(AlgorithmProcess);

  PrintUsage();

  //  Main loop
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    glfwMakeContextCurrent(window);
    Draw(window);

    glViewport(0, 0, g_width, g_height);

    g_first_frame = false;
  }

  // Cleanup
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();

  g_algorithm_process_finish = true;
  algorithm_thread.join();

  return 0;
}
