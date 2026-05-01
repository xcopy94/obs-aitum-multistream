#include "config-utils.hpp"
#include "multistream.hpp"
#include "obs-module.h"
#include "version.h"
#include <obs-frontend-api.h>
#include <obs-websocket-api.h>
#include <QDesktopServices>
#include <QGroupBox>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <util/config-file.h>
#include <util/platform.h>

extern "C" {
#include "file-updater.h"
}

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Aitum")
OBS_MODULE_USE_DEFAULT_LOCALE("aitum-multistream", "en-US")

static MultistreamDock *multistream_dock = nullptr;
static obs_websocket_vendor multistream_vendor = nullptr;

update_info_t *version_update_info = nullptr;

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

static void list_outputs_add_vertical(MultistreamDock *dock, obs_data_array_t *arr)
{
	if (!dock || !dock->vertical_outputs)
		return;

	obs_data_array_enum(dock->vertical_outputs,
		[](obs_data_t *data, void *param) {
			auto *pair = static_cast<std::pair<MultistreamDock *, obs_data_array_t *> *>(param);
			MultistreamDock *dock = pair->first;
			obs_data_array_t *arr = pair->second;

			const char *name = obs_data_get_string(data, "name");
			const char *server = obs_data_get_string(data, "stream_server");
			bool advanced = obs_data_get_bool(data, "advanced");
			bool active = false;

			struct calldata cd;
			calldata_init(&cd);
			calldata_set_string(&cd, "name", name ? name : "");
			if (proc_handler_call(obs_get_proc_handler(), "aitum_vertical_get_stream_output", &cd)) {
				obs_output_t *output = (obs_output_t *)calldata_ptr(&cd, "output");
				active = obs_output_active(output);
				obs_output_release(output);
			}
			calldata_free(&cd);

			append_output_metadata(arr, name, "vertical", active, server, true, advanced);
		},
		new std::pair<MultistreamDock *, obs_data_array_t *>{dock, arr});
}

static void websocket_list_outputs_impl(MultistreamDock *dock, obs_data_array_t *arr)
{
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

bool version_info_downloaded(void *param, struct file_download_data *file)
{
	UNUSED_PARAMETER(param);
	if (!file || !file->buffer.num)
		return true;

	QMetaObject::invokeMethod(multistream_dock, "ApiInfo", Q_ARG(QString, QString::fromUtf8((const char *)file->buffer.array)));

	if (version_update_info) {
		update_info_destroy(version_update_info);
		version_update_info = nullptr;
	}
	return true;
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Aitum-Multistream] loaded version %s", PROJECT_VERSION);

	const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	multistream_dock = new MultistreamDock(main_window);
	obs_frontend_add_dock_by_id("AitumMultistreamDock", obs_module_text("AitumMultistream"), multistream_dock);

	version_update_info = update_info_create_single("[Aitum Multistream]", "OBS", "https://api.aitum.tv/plugin/multi",
							version_info_downloaded, nullptr);
	return true;
}

void obs_module_post_load()
{
	if (multistream_dock) {
		multistream_dock->LoadVerticalOutputs(true);

		if (!multistream_vendor) {
			multistream_vendor = obs_websocket_register_vendor("aitum_multistream");
			if (multistream_vendor) {
				obs_websocket_vendor_register_request(multistream_vendor, "ListOutputs",
									      [](obs_data_t *, obs_data_t *response_data, void *priv_data) {
										      auto *dock = static_cast<MultistreamDock *>(priv_data);
										      obs_data_array_t *arr = obs_data_array_create();
										      websocket_list_outputs_impl(dock, arr);
										      obs_data_set_bool(response_data, "success", true);
										      obs_data_set_array(response_data, "outputs", arr);
										      obs_data_array_release(arr);
										  },
									      multistream_dock);

				obs_websocket_vendor_register_request(multistream_vendor, "StartOutput",
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
									      multistream_dock);

				obs_websocket_vendor_register_request(multistream_vendor, "StopOutput",
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
									      multistream_dock);

				obs_websocket_vendor_register_request(multistream_vendor, "ToggleOutput",
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
									      multistream_dock);
			}
		}
	}
}

void obs_module_unload()
{
	if (version_update_info) {
		update_info_destroy(version_update_info);
		version_update_info = nullptr;
	}
	if (multistream_dock) {
		delete multistream_dock;
	}
}

void RemoveWidget(QWidget *widget);

void RemoveLayoutItem(QLayoutItem *item)
{
	if (!item)
		return;
	RemoveWidget(item->widget());
	if (item->layout()) {
		while (QLayoutItem *item2 = item->layout()->takeAt(0))
			RemoveLayoutItem(item2);
	}
	delete item;
}

void RemoveWidget(QWidget *widget)
{
	if (!widget)
		return;
	if (widget->layout()) {
		auto l = widget->layout();
		QLayoutItem *item;
		while (l->count() > 0 && (item = l->takeAt(0))) {
			RemoveLayoutItem(item);
		}
		delete l;
	}
	delete widget;
}

// Output button styling
void MultistreamDock::outputButtonStyle(QPushButton *button)
{
	button->setMinimumHeight(24);

	std::string baseStyles = "min-width: 30px; padding: 2px 10px; border-width: 2px;";

	button->setStyleSheet(QString::fromUtf8(baseStyles + (button->isChecked() ? "background: rgb(0,210,153);" : "")));

	button->setIcon(button->isChecked() ? streamActiveIcon : streamInactiveIcon);
}
