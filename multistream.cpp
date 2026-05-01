#include "config-utils.hpp"
#include "multistream.hpp"
#include "obs-module.h"
#include "version.h"
#include "websocket-integration.hpp"
#include <obs-frontend-api.h>
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

update_info_t *version_update_info = nullptr;

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
		RegisterMultistreamWebSocketVendor(multistream_dock);
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
