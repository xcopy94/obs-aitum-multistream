#include "websocket-integration.hpp"

#include "multistream.hpp"
#include "obs-websocket-api.h"
#include <obs-module.h>
#include <obs-frontend-api.h>

static obs_websocket_vendor g_multistream_vendor = nullptr;

static void set_request_failure(obs_data_t *response_data, const char *message)
{
	obs_data_set_bool(response_data, "success", false);
	obs_data_set_string(response_data, "error", message);
}

static obs_output_t *find_output_by_name(MultistreamDock *dock, const char *name)
{
	if (!dock || !name || !*name)
		return nullptr;

	for (auto &entry : dock->outputs) {
		if (std::get<std::string>(entry) != name)
			continue;

		return obs_output_get_ref(std::get<obs_output_t *>(entry));
	}

	return nullptr;
}

static void append_output_metadata(obs_data_array_t *arr, const char *name, const char *kind, bool active,
				   const char *server, bool vertical, bool advanced)
{
	obs_data_t *item = obs_data_create();
	obs_data_set_string(item, "name", name ? name : "");
	obs_data_set_string(item, "kind", kind ? kind : "");
	obs_data_set_bool(item, "active", active);
	obs_data_set_string(item, "server", server ? server : "");
	obs_data_set_bool(item, "vertical", vertical);
	obs_data_set_bool(item, "advanced", advanced);
	obs_data_array_push_back(arr, item);
	obs_data_release(item);
}

struct VerticalListContext {
	MultistreamDock *dock;
	obs_data_array_t *arr;
};

static void append_vertical_output(obs_data_t *data, VerticalListContext *ctx)
{
	const char *name = obs_data_get_string(data, "name");
	const char *server = obs_data_get_string(data, "stream_server");
	bool advanced = obs_data_get_bool(data, "advanced");
	bool active = false;

	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "name", name ? name : "");
	if (proc_handler_call(obs_get_proc_handler(), "aitum_vertical_get_stream_output", &cd)) {
		obs_output_t *output = (obs_output_t *)calldata_ptr(&cd, "output");
		if (output) {
			active = obs_output_active(output);
			obs_output_release(output);
		}
	}
	calldata_free(&cd);

	append_output_metadata(ctx->arr, name, "vertical", active, server, true, advanced);
}

static void list_outputs_add_vertical(MultistreamDock *dock, obs_data_array_t *arr)
{
	if (!dock || !dock->vertical_outputs)
		return;

	VerticalListContext ctx{dock, arr};
	obs_data_array_enum(dock->vertical_outputs,
		[](obs_data_t *data, void *param) {
			auto *ctx = static_cast<VerticalListContext *>(param);
			append_vertical_output(data, ctx);
		},
		&ctx);
}

static void list_outputs_build(MultistreamDock *dock, obs_data_array_t *arr)
{
	if (!dock || !arr)
		return;

	for (auto &entry : dock->outputs) {
		const std::string &name = std::get<std::string>(entry);
		obs_output_t *output = std::get<obs_output_t *>(entry);
		obs_service_t *service = obs_output_get_service(output);
		const char *server = service ? obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL) : "";
		append_output_metadata(arr, name.c_str(), "main", obs_output_active(output), server ? server : "", false,
				       false);
		if (service)
			obs_service_release(service);
	}

	list_outputs_add_vertical(dock, arr);
}

static void register_list_outputs(obs_websocket_vendor vendor, MultistreamDock *dock)
{
	obs_websocket_vendor_register_request(vendor, "ListOutputs",
		[](obs_data_t *, obs_data_t *response_data, void *priv_data) {
			auto *dock = static_cast<MultistreamDock *>(priv_data);
			obs_data_array_t *arr = obs_data_array_create();
			list_outputs_build(dock, arr);
			obs_data_set_bool(response_data, "success", true);
			obs_data_set_array(response_data, "outputs", arr);
			obs_data_array_release(arr);
		},
		dock);
}

static void register_start_output(obs_websocket_vendor vendor, MultistreamDock *dock)
{
	obs_websocket_vendor_register_request(vendor, "StartOutput",
		[](obs_data_t *request_data, obs_data_t *response_data, void *priv_data) {
			auto *dock = static_cast<MultistreamDock *>(priv_data);
			const char *name = obs_data_get_string(request_data, "name");
			if (!name || !*name) {
				set_request_failure(response_data, "Missing required field: name");
				return;
			}

			obs_output_t *output = find_output_by_name(dock, name);
			if (!output) {
				set_request_failure(response_data, "Output not found");
				return;
			}

			bool ok = obs_output_start(output);
			bool active = obs_output_active(output);
			obs_output_release(output);

			obs_data_set_bool(response_data, "success", ok);
			obs_data_set_bool(response_data, "active", active);
			if (!ok)
				obs_data_set_string(response_data, "error", "obs_output_start failed");
		},
		dock);
}

static void register_stop_output(obs_websocket_vendor vendor, MultistreamDock *dock)
{
	obs_websocket_vendor_register_request(vendor, "StopOutput",
		[](obs_data_t *request_data, obs_data_t *response_data, void *priv_data) {
			auto *dock = static_cast<MultistreamDock *>(priv_data);
			const char *name = obs_data_get_string(request_data, "name");
			if (!name || !*name) {
				set_request_failure(response_data, "Missing required field: name");
				return;
			}

			obs_output_t *output = find_output_by_name(dock, name);
			if (!output) {
				set_request_failure(response_data, "Output not found");
				return;
			}

			obs_output_stop(output);
			bool active = obs_output_active(output);
			obs_output_release(output);

			obs_data_set_bool(response_data, "success", true);
			obs_data_set_bool(response_data, "active", active);
		},
		dock);
}

static void register_toggle_output(obs_websocket_vendor vendor, MultistreamDock *dock)
{
	obs_websocket_vendor_register_request(vendor, "ToggleOutput",
		[](obs_data_t *request_data, obs_data_t *response_data, void *priv_data) {
			auto *dock = static_cast<MultistreamDock *>(priv_data);
			const char *name = obs_data_get_string(request_data, "name");
			if (!name || !*name) {
				set_request_failure(response_data, "Missing required field: name");
				return;
			}

			obs_output_t *output = find_output_by_name(dock, name);
			if (!output) {
				set_request_failure(response_data, "Output not found");
				return;
			}

			bool active_before = obs_output_active(output);
			if (active_before)
				obs_output_stop(output);
			else
				obs_output_start(output);
			bool active_after = obs_output_active(output);
			obs_output_release(output);

			obs_data_set_bool(response_data, "success", true);
			obs_data_set_bool(response_data, "active", active_after);
		},
		dock);
}

void RegisterMultistreamWebSocketVendor(MultistreamDock *dock)
{
	if (!dock || g_multistream_vendor)
		return;

	g_multistream_vendor = obs_websocket_register_vendor("aitum_multistream");
	if (!g_multistream_vendor) {
		blog(LOG_WARNING, "[Aitum Multistream] Failed to register obs-websocket vendor");
		return;
	}

	register_list_outputs(g_multistream_vendor, dock);
	register_start_output(g_multistream_vendor, dock);
	register_stop_output(g_multistream_vendor, dock);
	register_toggle_output(g_multistream_vendor, dock);
}
