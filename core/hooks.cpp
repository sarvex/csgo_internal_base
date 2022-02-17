#include "cheat.h"
#include "../render/menu/menu.h"
#include "features/features.h"
#include "hooks.h"
#include "features/events.h"
#include "features/variables.h"

void hooks::initialize() noexcept
{
    D3DDEVICE_CREATION_PARAMETERS creation_params{ };
    interfaces::dx9_device->GetCreationParameters(&creation_params);
    if (creation_params.hFocusWindow) {
        game_window = creation_params.hFocusWindow;
        original_wnd_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wnd_proc)));
    }

    hook_func = memory::find_bytes(dll::game_overlay_renderer, "55 8B EC 51 8B 45 10 C7").cast<decltype(hook_func)>();
    unhook_func = memory::find_bytes(dll::game_overlay_renderer, "E8 ? ? ? ? 83 C4 08 FF 15").absolute<decltype(unhook_func)>();
    
    SET_VF_HOOK(interfaces::client, frame_stage_notify);
    SET_VF_HOOK(interfaces::client_mode, override_view);
    SET_VF_HOOK(interfaces::client_mode, create_move);
    SET_VF_HOOK(interfaces::client_mode, get_viewmodel_fov);
    SET_VF_HOOK(interfaces::engine, is_connected);
    SET_VF_HOOK(interfaces::material_system, override_config);
    SET_VF_HOOK(interfaces::model_render, draw_model_execute);
    SET_VF_HOOK(interfaces::panel, paint_traverse);
    SET_VF_HOOK(interfaces::bsp_query, list_leaves_in_box);
    SET_VF_HOOK(interfaces::surface, lock_cursor);

    SET_SIG_HOOK(dll::client, "55 8B EC 8B 4D 08 83 EC 18", cull_beam);

    SET_PROXY("CBaseEntity->m_bSpotted", spotted);

    // events::initialize({ "bullet_impact" });
}

void hooks::end() noexcept
{
    interfaces::client.restore();
    interfaces::client_mode.restore();
    interfaces::engine.restore();
    interfaces::material_system.restore();
    interfaces::model_render.restore();
    interfaces::panel.restore();
    interfaces::bsp_query.restore();
    interfaces::surface.restore();
    
    unhook_func(hooked_fns[fnv1a::ct("cull_beam")], false);
    
    // events::end();

    SetWindowLongPtrA(game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_wnd_proc));
}

LRESULT CALLBACK hooks::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    input::process(msg, wparam, lparam);
    
    if (input::is_key_active({ VK_INSERT, input::key_type::toggle }))
        menu::toggle();

    interfaces::input_system->enable_input(!menu::is_active);

    if (menu::is_active)
        return TRUE;

    return CallWindowProcA(original_wnd_proc, hwnd, msg, wparam, lparam);
}

void __fastcall hooks::frame_stage_notify::fn(se::client_dll* ecx, int, cs::frame_stage frame_stage)
{
    if (!interfaces::engine->is_in_game_and_connected())
        return original(ecx, frame_stage);

    static auto override_postprocessing_disable = *memory::find_bytes(dll::client, "83 EC 4C 80 3D").offset(0x5).cast<bool**>();

    switch (frame_stage) {
    case cs::frame_stage::render_start:
        *override_postprocessing_disable = config::get<bool>(vars::disable_postprocessing);
        break;
    case cs::frame_stage::net_update_post_data_update_end:
        cheat::local_player->update();
        break;
    }
    
    return original(ecx, frame_stage);
}

void __fastcall hooks::override_view::fn(se::client_mode* ecx, int, cs::view_setup* view)
{
    if (!cheat::local_player->update())
        return original(ecx, view);

    if (cheat::local_player->is_alive()) {
        if (!cheat::local_player->is_scoping())
            view->fov = config::get<float>(vars::fov);
    }

    return original(ecx, view);
}

bool __fastcall hooks::create_move::fn(se::client_mode* ecx, int, float input_sample_time, cs::user_cmd* cmd)
{
    if (!cmd || !cmd->number) // Return calls from CInput::ExtraMouseSample().
        return original(ecx, input_sample_time, cmd);

    if (original(ecx, input_sample_time, cmd))
        interfaces::prediction->set_local_view_angles(cmd->view_angles);

    cheat::local_player->update();
    cheat::cmd = cmd;

    auto stack_frame = memory::get_frame_address().dereference();
    auto& send_packet = *stack_frame.offset(-0x1c).cast<bool*>(); // Captured from CL_Move().
    
    if (config::get<bool>(vars::infinite_crouch))
        cmd->buttons.set(cs::cmd_button::bullrush);

    prediction::start(cmd);
    // aimbot::run(cmd);
    prediction::end();

    // Prevent view angles from being changed.
    return false;
}

float __fastcall hooks::get_viewmodel_fov::fn(se::client_mode* ecx, int)
{
    if (!cheat::local_player || (cheat::local_player && cheat::local_player->is_scoping()))
        return original(ecx);

    return config::get<float>(vars::viewmodel_fov);
}

bool __fastcall hooks::is_connected::fn(se::engine_client* ecx, int)
{
    static const auto is_loadout_allowed = memory::find_bytes(dll::client, "84 C0 75 05 B0 01 5F");
    if (config::get<bool>(vars::unlock_inventory) && _ReturnAddress() == is_loadout_allowed)
        return false;

    return original(ecx);
}

void __fastcall hooks::build_renderables_list::fn(se::leaf_system* ecx, int, cs::setup_render_info& info)
{
    original(ecx, info);
    
    constexpr int max_group_entities = 4096;
    auto& groups = info.render_list->render_groups;
    
    // use algorithm/ranges instead of looping?
    for (int i = 0; i < max_group_entities; i++) {
        // FIXME - pointer is invalid
        const auto renderable = groups[cs::render_group::opaque][i].renderable;
        if (!renderable)
            continue;
    
        const auto unknown = renderable->get_client_unknown();
        if (!unknown)
            continue;
    
        const auto entity = unknown->get_base_entity();
        if (!entity) {
            if (unknown->get_networkable()->get_client_class()->id == cs::class_id::particle_smoke_grenade)
                std::swap(groups[cs::render_group::translucent][i], groups[cs::render_group::opaque][i]);
            else 
                continue;
        }
        
        if (entity->is_player())
                std::swap(groups[cs::render_group::opaque][i], groups[cs::render_group::translucent_ignore_z][i]);
    }

}

bool __fastcall hooks::override_config::fn(se::material_system* ecx, int, cs::material_system_config* config, bool force_update)
{
    config->fullbright = config::get<bool>(vars::fullbright);
    return original(ecx, config, force_update);
}

static auto init_material() noexcept
{
    // return interfaces::material_system->find_material("debug/debugambientcube", cs::texture_group[cs::tg_model]);

    const auto kv = interfaces::mem_alloc->alloc<cs::key_values*>(sizeof(cs::key_values));
    kv->init("VertexLitGeneric");
    kv->load_from_buffer("celsius_default.vmt", R"#("VertexLitGeneric" {
        "$basetexture"  "vgui/white_additive"
        "$ignorez"      "0"
        "$model"		"1"
        "$flat"			"0"
        "$nocull"		"1"
        "$halflambert"	"1"
        "$nofog"		"1"
        "$wireframe"	"0"
    })#");

    return interfaces::material_system->create_material("celsius_default.vmt", kv);
}

static void override_material(bool xqz) noexcept
{
    static auto material = init_material();
    material->set_flag(cs::material_flag::ignore_z, xqz);
    material->modulate_color(0.8f, 0.3f, 0.0f);
    material->modulate_alpha(1.0f);
    interfaces::model_render->forced_material_override(material);
}

void __fastcall hooks::draw_model_execute::fn(se::model_render* ecx, int, cs::mat_render_context* render_ctx, const cs::draw_model_state& state, 
    const cs::model_render_info& info, mat3x4* bone_to_world)
{
    if (ecx->is_forced_material_override())
        return original(ecx, render_ctx, state, info, bone_to_world);

    const auto model_name = interfaces::model_info->get_model_name(info.model);
    if (strstr(model_name, "models/player")) {
        const auto player = interfaces::entity_list->get<cs::player*>(info.entity_index);
        if (player) {
            override_material(true);
            original(ecx, render_ctx, state, info, bone_to_world);
            ecx->forced_material_override(nullptr);
            override_material(false);
        }
    }

    original(ecx, render_ctx, state, info, bone_to_world);
    interfaces::model_render->forced_material_override(nullptr);
}

void __fastcall hooks::paint_traverse::fn(se::panel* ecx, int, cs::vpanel panel, bool force_repaint, bool allow_force)
{
    static bool once = []() { return render::initialize(); }();
    static cs::vpanel tools_handle = []() { return interfaces::vgui->get_panel(cs::vgui_panel::tools); }();

    if (panel == tools_handle) {
        render::text(render::fonts::watermark, { 15, 15 }, L"hello");
        menu::run();
    }
    
    return original(ecx, panel, force_repaint, allow_force);
}

int __fastcall hooks::list_leaves_in_box::fn(se::spatial_query* ecx, int, const vec3& mins, const vec3& maxs, unsigned short* list, int list_max)
{
    static const auto insert_into_tree = memory::find_bytes(dll::client, "89 44 24 14 EB 08 C7 44 24 ? ? ? ? ? 8B 45");

    constexpr float max_coord = 16384.0f;
    constexpr vec3 new_maxs = { max_coord, max_coord, max_coord };
    constexpr vec3 new_mins = { -max_coord, -max_coord, -max_coord };

    if (_ReturnAddress() != insert_into_tree)
        return original(ecx, mins, maxs, list, list_max);

    const auto info = *reinterpret_cast<cs::renderable_info**>(reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) + 0x14);
    const auto entity = info->renderable->get_client_unknown()->get_base_entity();
    if (!entity)
        return original(ecx, mins, maxs, list, list_max);
    
    // Force into translucent group.
    if (entity->is_player()) {
        info->flags.unset(cs::render_flag::force_opaque_pass);
        info->flags.set(cs::render_flag::bounds_always_recompute); // Setting IS_SPRITE (0x80) is unnecessary
    }

    // Return with largest possible world bounds to draw player models over longer distances
    return original(ecx, new_mins, new_maxs, list, list_max);
}

void __fastcall hooks::lock_cursor::fn(se::surface* ecx, int)
{
    return menu::is_active ? ecx->unlock_cursor() : original(ecx);
}

// See https://github.com/perilouswithadollarsign/cstrike15_src/blob/master/game/client/view_beams.cpp#L1070
int __stdcall hooks::cull_beam::fn(const vec3& start, const vec3& end, int pvs_only)
{
    return 1;
}

void hooks::spotted::proxy(cs::recv_proxy_data* data, void* arg0, void* arg1)
{
    if (config::get<bool>(vars::radar_reveal))
        data->value.integer = 1;

    return original(data, arg0, arg1);
}

#undef SET_SIG_HOOK
#undef SET_VF_HOOK
#undef SET_IAT_HOOK
