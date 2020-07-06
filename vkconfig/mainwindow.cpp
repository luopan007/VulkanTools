/*
 * Copyright (c) 2020 Valve Corporation
 * Copyright (c) 2020 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Authors:
 * - Richard S. Wright Jr. <richard@lunarg.com>
 * - Christophe Riccio <christophe@lunarg.com>
 */

#include <QProcess>
#include <QDir>
#include <QMessageBox>
#include <QFile>
#include <QFrame>
#include <QComboBox>
#include <QVariant>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QLineEdit>
#include <QDesktopServices>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "dlgabout.h"
#include "dlgvulkananalysis.h"
#include "dlgvulkaninfo.h"
#include "dlgprofileeditor.h"
#include "dlgcreateassociation.h"
#include "dlgcustompaths.h"
#include "configurator.h"
#include "preferences.h"

// This is what happens when programmers can touch type....
bool been_warned_about_old_loader = false;

#define EDITOR_CAPTION_EMPTY "Configuration Layer Settings"

static const int LAUNCH_COLUMN0_SIZE = 240;
static const int LAUNCH_COLUMN2_SIZE = 32;
static const int LAUNCH_SPACING_SIZE = 2;
#ifdef __APPLE__
static const int LAUNCH_ROW_HEIGHT = 24;
#else
static const int LAUNCH_ROW_HEIGHT = 28;
#endif

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui_(new Ui::MainWindow) {
    ui_->setupUi(this);
    ui_->launchTree->installEventFilter(this);
    ui_->profileTree->installEventFilter(this);

    selected_configuration_item_ = nullptr;
    vk_via_ = nullptr;
    vk_info_ = nullptr;
    help_ = nullptr;
    launch_application_ = nullptr;
    log_file_ = nullptr;
    pLaunchAppsCombo = nullptr;
    pLaunchArguments = nullptr;

    ///////////////////////////////////////////////
    Configurator &configurator = Configurator::Get();

    // We need to resetup the new profile for consistency sake.
    QSettings settings;
    QString last_configuration = settings.value(VKCONFIG_KEY_ACTIVEPROFILE, QString("Validation - Standard")).toString();
    Configuration *current_configuration = configurator.FindConfiguration(last_configuration);
    if (configurator.override_active) ChangeActiveConfiguration(current_configuration);

    LoadConfigurationList();
    SetupLaunchTree();

    connect(ui_->actionAbout, SIGNAL(triggered(bool)), this, SLOT(aboutVkConfig(bool)));
    connect(ui_->actionVulkan_Info, SIGNAL(triggered(bool)), this, SLOT(toolsVulkanInfo(bool)));
    connect(ui_->actionHelp, SIGNAL(triggered(bool)), this, SLOT(helpShowHelp(bool)));

    connect(ui_->actionCustom_Layer_Paths, SIGNAL(triggered(bool)), this, SLOT(toolsSetCustomPaths(bool)));

    connect(ui_->actionVulkan_Installation, SIGNAL(triggered(bool)), this, SLOT(toolsVulkanInstallation(bool)));
    connect(ui_->actionRestore_Default_Configurations, SIGNAL(triggered(bool)), this, SLOT(toolsResetToDefault(bool)));

    connect(ui_->profileTree, SIGNAL(itemChanged(QTreeWidgetItem *, int)), this, SLOT(profileItemChanged(QTreeWidgetItem *, int)));
    connect(ui_->profileTree, SIGNAL(currentItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)), this,
            SLOT(profileTreeChanged(QTreeWidgetItem *, QTreeWidgetItem *)));
    connect(ui_->profileTree, SIGNAL(itemClicked(QTreeWidgetItem *, int)), this,
            SLOT(OnConfigurationTreeClicked(QTreeWidgetItem *, int)));

    connect(ui_->layerSettingsTree, SIGNAL(itemExpanded(QTreeWidgetItem *)), this, SLOT(editorExpanded(QTreeWidgetItem *)));
    connect(ui_->layerSettingsTree, SIGNAL(itemClicked(QTreeWidgetItem *, int)), this,
            SLOT(OnConfigurationSettingsTreeClicked(QTreeWidgetItem *, int)));

    connect(ui_->launchTree, SIGNAL(itemCollapsed(QTreeWidgetItem *)), this, SLOT(launchItemCollapsed(QTreeWidgetItem *)));
    connect(ui_->launchTree, SIGNAL(itemExpanded(QTreeWidgetItem *)), this, SLOT(launchItemExpanded(QTreeWidgetItem *)));

    ui_->pushButtonAppList->setEnabled(configurator.override_application_list_only);
    ui_->checkBoxApplyList->setChecked(configurator.override_application_list_only);
    ui_->checkBoxPersistent->setChecked(configurator.override_permanent);

    if (configurator.override_active) {
        ui_->radioOverride->setChecked(true);
        ui_->groupBoxProfiles->setEnabled(true);
        ui_->groupBoxEditor->setEnabled(true);
    } else {
        ui_->radioFully->setChecked(true);
        ui_->checkBoxApplyList->setEnabled(false);
        ui_->checkBoxPersistent->setEnabled(false);
        ui_->pushButtonAppList->setEnabled(false);
        ui_->groupBoxProfiles->setEnabled(false);
        ui_->groupBoxEditor->setEnabled(false);
    }

    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());

    // All else is done, highlight and activeate the current profile on startup
    Configuration *pActive = configurator.GetActiveConfiguration();
    if (pActive != nullptr) {
        for (int i = 0; i < ui_->profileTree->topLevelItemCount(); i++) {
            ContigurationListItem *pItem = dynamic_cast<ContigurationListItem *>(ui_->profileTree->topLevelItem(i));
            if (pItem != nullptr)
                if (pItem->configuration == pActive) {  // Ding ding ding... we have a winner
                    ui_->profileTree->setCurrentItem(pItem);
                }
        }
    }

    ui_->logBrowser->append("Vulkan Development Status:");
    ui_->logBrowser->append(configurator.CheckVulkanSetup());
}

MainWindow::~MainWindow() { delete ui_; }

///////////////////////////////////////////////////////////////////////////////
// Load or refresh the list of profiles. Any profile that uses a layer that
// is not detected on the system is disabled.
void MainWindow::LoadConfigurationList() {
    // There are lots of ways into this, and in none of them
    // can we have an active editor running.
    settings_tree_manager_.CleanupGUI();
    ui_->profileTree->blockSignals(true);  // No signals firing off while we do this
    ui_->profileTree->clear();

    // Who is the currently active profile?
    QSettings settings;
    QString active_configuration_name = settings.value(VKCONFIG_KEY_ACTIVEPROFILE).toString();

    Configurator &configurator = Configurator::Get();

    for (int i = 0; i < configurator.available_configurations.size(); i++) {
        // Add to list
        ContigurationListItem *item = new ContigurationListItem();
        item->configuration = configurator.available_configurations[i];
        ui_->profileTree->addTopLevelItem(item);
        item->setText(1, configurator.available_configurations[i]->name);
        item->setToolTip(1, configurator.available_configurations[i]->description);
        item->radio_button = new QRadioButton();
        item->radio_button->setText("");

        if (!configurator.available_configurations[i]->IsValid()) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->radio_button->setEnabled(false);
            item->setToolTip(1, "Missing Vulkan Layer to use this configuration, try to add Custom Path to locate the layers");
        }
        if (active_configuration_name == configurator.available_configurations[i]->name) {
            item->radio_button->setChecked(true);
        }

        item->setFlags(item->flags() | Qt::ItemIsEditable);
        ui_->profileTree->setItemWidget(item, 0, item->radio_button);
        connect(item->radio_button, SIGNAL(clicked(bool)), this, SLOT(profileItemClicked(bool)));
    }

    ui_->profileTree->blockSignals(false);
    ChangeActiveConfiguration(configurator.GetActiveConfiguration());
    ui_->profileTree->setColumnWidth(0, 24);
    ui_->profileTree->resizeColumnToContents(1);
}

//////////////////////////////////////////////////////////
/// \brief MainWindow::on_radioFully_clicked
// No override at all, fully controlled by the application
void MainWindow::on_radioFully_clicked(void) {
    ui_->checkBoxApplyList->setEnabled(false);
    ui_->checkBoxPersistent->setEnabled(false);

    Configurator &configurator = Configurator::Get();

    configurator.override_active = false;
    ui_->groupBoxProfiles->setEnabled(false);
    ui_->groupBoxEditor->setEnabled(false);

    ui_->pushButtonAppList->setEnabled(false);

    configurator.SaveSettings();
    ChangeActiveConfiguration(nullptr);
}

//////////////////////////////////////////////////////////
/// \brief MainWindow::GetCheckedItem
/// \return
/// Okay, because we are using custom controls, some of
/// the signaling is not happening as expected. So, we cannot
/// always get an accurate answer to the currently selected
/// item, but we do often need to know what has been checked
/// when an event occurs. This unambigously answers that question.
ContigurationListItem *MainWindow::GetCheckedItem(void) {
    // Just go through all the top level items
    for (int i = 0; i < ui_->profileTree->topLevelItemCount(); i++) {
        ContigurationListItem *pItem = dynamic_cast<ContigurationListItem *>(ui_->profileTree->topLevelItem(i));

        if (pItem != nullptr)
            if (pItem->radio_button->isChecked()) return pItem;
    }

    return nullptr;
}

//////////////////////////////////////////////////////////
/// Use the active profile as the override
void MainWindow::on_radioOverride_clicked() {
    Configurator &configurator = Configurator::Get();

    bool bUse = (!configurator.has_old_loader || !been_warned_about_old_loader);
    ui_->checkBoxApplyList->setEnabled(bUse);
    ui_->pushButtonAppList->setEnabled(bUse && configurator.override_application_list_only);

    ui_->checkBoxPersistent->setEnabled(true);
    configurator.override_active = true;
    ui_->groupBoxProfiles->setEnabled(true);
    ui_->groupBoxEditor->setEnabled(true);
    configurator.SaveSettings();

    // This just doesn't work. Make a function to look for the radio button checked.
    ContigurationListItem *pProfileItem = GetCheckedItem();
    if (pProfileItem == nullptr)
        ChangeActiveConfiguration(nullptr);
    else
        ChangeActiveConfiguration(pProfileItem->configuration);
}

///////////////////////////////////////////////////////////////////////
// We want to apply to just the app list... hang on there. Doe we have
// the new loader?
void MainWindow::on_checkBoxApplyList_clicked() {
    Configurator &configurator = Configurator::Get();

    if (configurator.has_old_loader && !been_warned_about_old_loader) {
        uint32_t version = configurator.vulkan_instance_version;
        QString message;
        message = QString().asprintf(
            "The detected Vulkan Loader version is %d.%d.%d but version 1.2.141 or newer is required in order to apply layers "
            "override to only a selected list of Vulkan applications.\n\n<br><br>"
            "Get the latest Vulkan Runtime from <a href='https://vulkan.lunarg.com/sdk/home'>HERE.</a> to use this feature.",
            VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
        QMessageBox alert(this);
        alert.setTextFormat(Qt::RichText);
        alert.setText(message);
        alert.setIcon(QMessageBox::Warning);
        alert.setWindowTitle(tr("Layers override of a selected list of Vulkan Applications is not available"));
        alert.exec();

        ui_->pushButtonAppList->setEnabled(false);
        ui_->checkBoxApplyList->setEnabled(false);
        ui_->checkBoxApplyList->setChecked(false);
        QString messageToolTip;
        messageToolTip =
            QString().asprintf("The detected Vulkan loader version is %d.%d.%d but version 1.2.141 or newer is required",
                               VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
        ui_->checkBoxApplyList->setToolTip(messageToolTip);
        ui_->pushButtonAppList->setToolTip(messageToolTip);
        been_warned_about_old_loader = true;
    }

    configurator.override_application_list_only = ui_->checkBoxApplyList->isChecked();
    configurator.SaveSettings();
    ui_->pushButtonAppList->setEnabled(configurator.override_application_list_only);
}

//////////////////////////////////////////////////////////
void MainWindow::on_checkBoxPersistent_clicked(void) {
    Configurator &configurator = Configurator::Get();

    configurator.override_permanent = ui_->checkBoxPersistent->isChecked();
    configurator.SaveSettings();
}

//////////////////////////////////////////////////////////
void MainWindow::toolsResetToDefault(bool bChecked) {
    (void)bChecked;

    // Let make sure...
    QMessageBox msg;
    msg.setIcon(QMessageBox::Warning);
    msg.setWindowTitle(tr("Restoring and Resetting all Layers Configurations to default"));
    msg.setText(
        tr("You are about to delete all the user-defined configurations and resetting all default configurations to their default "
           "state.\n\n"
           "Are you sure you want to continue?"));
    msg.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msg.setDefaultButton(QMessageBox::Yes);
    if (msg.exec() == QMessageBox::No) return;

    Configurator &configurator = Configurator::Get();

    // Clear the current profile as we may be about to remove it.
    configurator.SetActiveConfiguration(nullptr);

    // Delete all the *.json files in the storage folder
    QDir dir(configurator.GetPath(Configurator::ConfigurationPath));
    dir.setFilter(QDir::Files | QDir::NoSymLinks);
    dir.setNameFilters(QStringList() << "*.json");
    QFileInfoList configuration_files = dir.entryInfoList();

    // Loop through all the profiles found and remove them
    for (int i = 0; i < configuration_files.size(); i++) {
        QFileInfo info = configuration_files.at(i);
        if (info.absoluteFilePath().contains("applist.json")) continue;
        remove(info.filePath().toUtf8().constData());
    }

    // Reset to recopy from resource file
    QSettings settings;
    settings.setValue(VKCONFIG_KEY_FIRST_RUN, true);

    // Now we need to kind of restart everything
    settings_tree_manager_.CleanupGUI();
    configurator.LoadAllConfigurations();

    // Find the Standard Validation and make it current if we are active
    Configuration *pNewActiveProfile = configurator.FindConfiguration(QString("Validation - Standard"));
    if (configurator.override_active) ChangeActiveConfiguration(pNewActiveProfile);

    LoadConfigurationList();

    // Active or not, set it in the tree so we can see the settings.
    for (int i = 0; i < ui_->profileTree->topLevelItemCount(); i++) {
        ContigurationListItem *pItem = dynamic_cast<ContigurationListItem *>(ui_->profileTree->topLevelItem(i));
        if (pItem != nullptr)
            if (pItem->configuration == pNewActiveProfile) ui_->profileTree->setCurrentItem(pItem);
    }

    configurator.FindVkCube();
    ResetLaunchOptions();

    ui_->logBrowser->clear();
    ui_->logBrowser->append("Vulkan Development Status:");
    ui_->logBrowser->append(configurator.CheckVulkanSetup());

    if (configurator.override_active) {
        ui_->radioOverride->setChecked(true);
        ui_->checkBoxApplyList->setEnabled(true);
        ui_->checkBoxPersistent->setEnabled(true);
        ui_->groupBoxProfiles->setEnabled(true);
        ui_->groupBoxEditor->setEnabled(true);
    } else {
        ui_->radioFully->setChecked(true);
        ui_->checkBoxApplyList->setEnabled(false);
        ui_->checkBoxPersistent->setEnabled(false);
        ui_->pushButtonAppList->setEnabled(false);
        ui_->groupBoxProfiles->setEnabled(false);
        ui_->groupBoxEditor->setEnabled(false);
    }
}

////////////////////////////////////////////////////////////////////////////
/// \brief MainWindow::profileItemClicked
/// \param bChecked
/// Thist signal actually comes from the radio button
void MainWindow::profileItemClicked(bool bChecked) {
    (void)bChecked;
    // Someone just got checked, they are now the current profile
    // This pointer will only be valid if it's one of the elements with
    // the radio button
    ContigurationListItem *pProfileItem = GetCheckedItem();
    if (pProfileItem == nullptr) return;

    Configurator &configurator = Configurator::Get();

    // Do we go ahead and activate it?
    if (configurator.override_active) {
        ChangeActiveConfiguration(pProfileItem->configuration);
    }
}

/////////////////////////////////////////////////////////////////////////////
/// An item has been changed. Check for edit of the items name (profile name)
void MainWindow::profileItemChanged(QTreeWidgetItem *pItem, int nCol) {
    // This pointer will only be valid if it's one of the elements with
    // the radio button
    ContigurationListItem *configuration_item = dynamic_cast<ContigurationListItem *>(pItem);
    if (configuration_item == nullptr) return;

    if (nCol == 1) {  // Profile name
        Configurator &configurator = Configurator::Get();

        // We are renaming the file. Just delete the old one and save this
        const QString full_path =
            configurator.GetPath(Configurator::ConfigurationPath) + "/" + configuration_item->configuration->file;
        remove(full_path.toUtf8().constData());

        configuration_item->configuration->name = configuration_item->text(1);
        configuration_item->configuration->file = configuration_item->text(1) + QString(".json");
        configurator.SaveConfiguration(configuration_item->configuration);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
/// This gets called with keyboard selections and clicks that do not necessarily
/// result in a radio button change (but it may). So we need to do two checks here, one
/// for the radio button, and one to change the editor/information at lower right.
void MainWindow::profileTreeChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous) {
    (void)previous;
    settings_tree_manager_.CleanupGUI();
    // This pointer will only be valid if it's one of the elements with
    // the radio button
    ContigurationListItem *pProfileItem = dynamic_cast<ContigurationListItem *>(current);
    if (pProfileItem == nullptr) return;

    if (!Preferences::Get().use_separated_select_and_activate) {
        pProfileItem->radio_button->setChecked(true);
        ChangeActiveConfiguration(pProfileItem->configuration);
    }

    settings_tree_manager_.CreateGUI(ui_->layerSettingsTree, pProfileItem->configuration);
    QString title = pProfileItem->configuration->name;
    title += " Settings";
    ui_->groupBoxEditor->setTitle(title);
    ui_->layerSettingsTree->resizeColumnToContents(0);
}

////////////////////////////////////////////////////
// Unused flag, just display the about Qt dialog
void MainWindow::aboutVkConfig(bool checked) {
    (void)checked;
    dlgAbout dlg(this);
    dlg.exec();
}

//////////////////////////////////////////////////////////
/// Create the VulkanInfo dialog if it doesn't already
/// exits & show it.
void MainWindow::toolsVulkanInfo(bool checked) {
    (void)checked;

    if (vk_info_ == nullptr) vk_info_ = new dlgVulkanInfo(this);

    vk_info_->RunTool();
}

//////////////////////////////////////////////////////////
/// Create the VulkanTools dialog if it doesn't already
/// exist & show it.
void MainWindow::toolsVulkanInstallation(bool checked) {
    (void)checked;
    if (vk_via_ == nullptr) vk_via_ = new dlgVulkanAnalysis(this);

    vk_via_->RunTool();
}

////////////////////////////////////////////////////////////////
/// Show help, which is just a rich text file
void MainWindow::helpShowHelp(bool checked) {
    (void)checked;
    if (help_ == nullptr) help_ = new dlgHelp(nullptr);

    help_->show();
}

////////////////////////////////////////////////////////////////
/// The only thing we need to do here is clear the profile if
/// the user does not want it active.
void MainWindow::closeEvent(QCloseEvent *event) {
    Configurator &configurator = Configurator::Get();
    if (!configurator.override_permanent) configurator.SetActiveConfiguration(nullptr);

    configurator.SaveOverriddenApplicationList();
    configurator.SaveSettings();

    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
    QMainWindow::closeEvent(event);
}

////////////////////////////////////////////////////////////////
/// \brief MainWindow::resizeEvent
/// \param pEvent
/// Resizing needs a little help. Yes please, there has to be
/// a better way of doing this.
void MainWindow::resizeEvent(QResizeEvent *event) {
    if (event != nullptr) event->accept();
}

/////////////////////////////////////////////////////////////
void MainWindow::showEvent(QShowEvent *event) {
    //  resizeEvent(nullptr); // Fake to get controls to do the right thing
    event->accept();
}

///////////////////////////////////////////////////////////////////////////////
/// \brief MainWindow::on_pushButtonAppList_clicked
/// Edit the list of apps that can be filtered.
void MainWindow::on_pushButtonAppList_clicked() {
    dlgCreateAssociation dlg(this);
    dlg.exec();

    Configurator &configurator = Configurator::Get();

    if (Preferences::Get().use_last_selected_application_in_launcher) {
        configurator.SelectLaunchApplication(dlg.GetSelectedLaunchApplicationIndex());
    }

    configurator.SaveOverriddenApplicationList();
    ResetLaunchOptions();

    // Also, we may have changed exclusion flags, so reset override
    Configuration *pCurr = configurator.GetActiveConfiguration();
    if (pCurr != nullptr) configurator.SetActiveConfiguration(pCurr);
}

///////////////////////////////////////////////////////////////////////////////
/// Just resave the list anytime we go into the editor
void MainWindow::on_pushButtonEditProfile_clicked() {
    // Who is selected?
    ContigurationListItem *item = dynamic_cast<ContigurationListItem *>(ui_->profileTree->currentItem());
    if (item == nullptr) return;

    // Save current state before we go in
    settings_tree_manager_.CleanupGUI();

    dlgProfileEditor dlg(this, item->configuration);
    dlg.exec();
    // pItem will be invalid after LoadProfileList, but I still
    // need the pointer to the profile
    QString editedProfileName = item->configuration->name;

    Configurator::Get().LoadAllConfigurations();
    LoadConfigurationList();

    // Reset the current item
    for (int i = 0; i < ui_->profileTree->topLevelItemCount(); i++) {
        item = dynamic_cast<ContigurationListItem *>(ui_->profileTree->topLevelItem(i));
        if (item != nullptr)
            if (item->configuration->name == editedProfileName) {
                ui_->profileTree->setCurrentItem(item);
                //                settingsTreeManager.CreateGUI(ui->layerSettingsTree, pProfile);
                break;
            }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Create a new blank profile
void MainWindow::NewClicked() {
    Configurator &configurator = Configurator::Get();

    Configuration *pNewProfile = configurator.CreateEmptyConfiguration();
    dlgProfileEditor dlg(this, pNewProfile);
    if (QDialog::Accepted == dlg.exec()) {
        configurator.LoadAllConfigurations();
        LoadConfigurationList();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \brief MainWindow::addCustomPaths
/// Allow addition or removal of custom layer paths. Afterwards reset the list
/// of loaded layers, but only if something was changed.
void MainWindow::addCustomPaths() {
    // Get the tree state and clear it.
    settings_tree_manager_.CleanupGUI();

    dlgCustomPaths dlg(this);
    dlg.exec();
    LoadConfigurationList();  // Force a reload
}

//////////////////////////////////////////////////////////////////////////////
/// Remove the currently selected user defined profile.
void MainWindow::RemoveClicked(ContigurationListItem *item) {
    // Let make sure...
    QMessageBox msg;
    msg.setInformativeText(item->configuration->name);
    msg.setText(tr("Are you sure you want to remove this configuration?"));
    msg.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msg.setDefaultButton(QMessageBox::Yes);
    if (msg.exec() == QMessageBox::No) return;

    // What if this is the active profile? We will go boom boom soon...
    Configurator &configurator = Configurator::Get();
    if (configurator.GetActiveConfiguration() == item->configuration) configurator.SetActiveConfiguration(nullptr);

    // Delete the file
    const QString full_path = configurator.GetPath(Configurator::ConfigurationPath) + "/" + item->configuration->file;
    remove(full_path.toUtf8().constData());

    // Reload profiles
    configurator.LoadAllConfigurations();
    LoadConfigurationList();
}

/////////////////////////////////////////////////////////////////////////////
void MainWindow::RenameClicked(ContigurationListItem *item) { ui_->profileTree->editItem(item, 1); }

/////////////////////////////////////////////////////////////////////////////
// Copy the current configuration
void MainWindow::DuplicateClicked(ContigurationListItem *item) {
    QString qsNewName = item->configuration->name;
    qsNewName += "2";
    settings_tree_manager_.CleanupGUI();
    item->configuration->name = qsNewName;

    Configurator &configurator = Configurator::Get();
    configurator.SaveConfiguration(item->configuration);
    configurator.LoadAllConfigurations();
    LoadConfigurationList();

    // Good enough? Nope, I want to select it and edit the name.
    // Find it.
    for (int i = 0; i < ui_->profileTree->topLevelItemCount(); i++) {
        ContigurationListItem *pItem = dynamic_cast<ContigurationListItem *>(ui_->profileTree->topLevelItem(i));
        if (pItem->configuration->name == qsNewName) {
            ui_->profileTree->editItem(pItem, 1);
            return;
        }
    }
}

/////////////////////////////////////////////////////////////////////////////
// Import a configuration file. File copy followed by a reload.
void MainWindow::ImportClicked(ContigurationListItem *item) {
    (void)item;  // We don't need this

    Configurator &configurator = Configurator::Get();

    QString full_suggested_path = configurator.GetPath(Configurator::LastImportPath);
    QString full_import_path =
        QFileDialog::getOpenFileName(this, "Import Layers Configuration File", full_suggested_path, "*.json");
    if (full_import_path.isEmpty()) return;

    settings_tree_manager_.CleanupGUI();
    Configurator::Get().ImportConfiguration(full_import_path);
    LoadConfigurationList();
    configurator.SaveSettings();
}

/////////////////////////////////////////////////////////////////////////////
// Export a configuration file. Basically just a file copy
void MainWindow::ExportClicked(ContigurationListItem *item) {
    Configurator &configurator = Configurator::Get();

    // Where to put it and what to call it
    QString full_suggested_path = configurator.GetPath(Configurator::LastExportPath) + "/" + item->configuration->file;
    QString full_export_path =
        QFileDialog::getSaveFileName(this, "Export Layers Configuration File", full_suggested_path, "*.json");
    if (full_export_path.isEmpty()) return;

    configurator.ExportConfiguration(item->configuration->file, full_export_path);
    configurator.SaveSettings();
}

/////////////////////////////////////////////////////////////////////////////
// Export a configuration file. Basically just a file copy
void MainWindow::EditCustomPathsClicked(ContigurationListItem *item) {
    (void)item;
    addCustomPaths();
    LoadConfigurationList();  // Force a reload
}

void MainWindow::toolsSetCustomPaths(bool bChecked) {
    (void)bChecked;
    addCustomPaths();
    LoadConfigurationList();  // Force a reload
}

/////////////////////////////////////////////////////////////////////////////
/// Update "decorations": window caption, (Active) status in list
void MainWindow::ChangeActiveConfiguration(Configuration *configuration) {
    Configurator &configurator = Configurator::Get();

    if (configuration == nullptr || !configurator.override_active) {
        configurator.SetActiveConfiguration(nullptr);

        setWindowTitle("Vulkan Configurator <VULKAN APPLICATION CONTROLLED>");
    } else {
        QString newCaption = configuration->name;
        if (!configuration->IsValid()) newCaption += " (DISABLED)";
        newCaption += " - Vulkan Configurator ";
        configurator.SetActiveConfiguration(configuration);
        newCaption += "<VULKAN APPLICATIONS OVERRIDDEN>";

        setWindowTitle(newCaption);
    }
}

void MainWindow::editorExpanded(QTreeWidgetItem *item) {
    (void)item;
    ui_->layerSettingsTree->resizeColumnToContents(0);
}

void MainWindow::profileItemExpanded(QTreeWidgetItem *item) {
    (void)item;
    ui_->layerSettingsTree->resizeColumnToContents(0);
    ui_->layerSettingsTree->resizeColumnToContents(1);
}

void MainWindow::OnConfigurationTreeClicked(QTreeWidgetItem *item, int column) {
    (void)item;

    Configurator::Get().CheckApplicationRestart();
}

void MainWindow::OnConfigurationSettingsTreeClicked(QTreeWidgetItem *item, int column) {
    (void)item;
    (void)column;

    Configurator::Get().CheckApplicationRestart();
}

///////////////////////////////////////////////////////////////////
/// Reload controls for launch control
void MainWindow::ResetLaunchOptions() {
    Configurator &configurator = Configurator::Get();
    ui_->pushButtonLaunch->setEnabled(!configurator.overridden_application_list.empty());

    // Reload launch apps selections
    pLaunchAppsCombo->blockSignals(true);
    pLaunchAppsCombo->clear();

    for (int i = 0; i < configurator.overridden_application_list.size(); i++) {
        pLaunchAppsCombo->addItem(configurator.overridden_application_list[i]->executable_path);
    }

    if (configurator.overridden_application_list.isEmpty()) {
        pLaunchArguments->setText("");
        pLaunchWorkingFolder->setText("");
        pLaunchLogFileEdit->setText("");
        return;
    }

    int launch_application_index = configurator.GetLaunchApplicationIndex();
    assert(launch_application_index >= 0);

    configurator.SelectLaunchApplication(launch_application_index);
    pLaunchAppsCombo->setCurrentIndex(launch_application_index);

    // Reset working folder and command line choices
    pLaunchArguments->setText(configurator.overridden_application_list[launch_application_index]->arguments);
    pLaunchWorkingFolder->setText(configurator.overridden_application_list[launch_application_index]->working_folder);
    pLaunchLogFileEdit->setText(configurator.overridden_application_list[launch_application_index]->log_file);
    pLaunchAppsCombo->blockSignals(false);
}

///////////////////////////////////////////////////////////////////
/// Launch and log area
void MainWindow::SetupLaunchTree() {
    /////////////////////////////////////////////////////////////////
    // Executable
    QTreeWidgetItem *pLauncherParent = new QTreeWidgetItem();
    pLauncherParent->setText(0, "Executable Path");
    ui_->launchTree->addTopLevelItem(pLauncherParent);

    pLaunchAppsCombo = new QComboBox();
    pLaunchAppsCombo->setMinimumHeight(LAUNCH_ROW_HEIGHT);
    pLaunchAppsCombo->setMaximumHeight(LAUNCH_ROW_HEIGHT);
    ui_->launchTree->setItemWidget(pLauncherParent, 1, pLaunchAppsCombo);

    pLuanchAppBrowseButton = new QPushButton();
    pLuanchAppBrowseButton->setText("...");
    pLuanchAppBrowseButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    pLuanchAppBrowseButton->setMaximumWidth(LAUNCH_COLUMN2_SIZE);
    pLuanchAppBrowseButton->setMinimumHeight(LAUNCH_ROW_HEIGHT);
    pLuanchAppBrowseButton->setMaximumHeight(LAUNCH_ROW_HEIGHT);
    ui_->launchTree->setItemWidget(pLauncherParent, 2, pLuanchAppBrowseButton);
    connect(pLaunchAppsCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(launchItemChanged(int)));
    connect(pLuanchAppBrowseButton, SIGNAL(clicked()), this, SLOT(on_pushButtonAppList_clicked()));

    //////////////////////////////////////////////////////////////////
    // Working folder
    QTreeWidgetItem *pLauncherFolder = new QTreeWidgetItem();
    pLauncherFolder->setText(0, "Working Directory");
    pLauncherParent->addChild(pLauncherFolder);

    pLaunchWorkingFolder = new QLineEdit();
    pLaunchWorkingFolder->setMinimumHeight(LAUNCH_ROW_HEIGHT);
    pLaunchWorkingFolder->setMaximumHeight(LAUNCH_ROW_HEIGHT);
    ui_->launchTree->setItemWidget(pLauncherFolder, 1, pLaunchWorkingFolder);
    pLaunchWorkingFolder->setReadOnly(false);

    // Comming soon
    //    pLaunchWorkingFolderButton = new QPushButton();
    //    pLaunchWorkingFolderButton->setText("...");
    //    pLaunchWorkingFolderButton->setMinimumWidth(32);
    //    ui->launchTree->setItemWidget(pLauncherFolder, 2, pLaunchWorkingFolderButton);

    //////////////////////////////////////////////////////////////////
    // Command line arguments
    QTreeWidgetItem *pLauncherCMD = new QTreeWidgetItem();
    pLauncherCMD->setText(0, "Command-line Arguments");
    pLauncherParent->addChild(pLauncherCMD);

    pLaunchArguments = new QLineEdit();
    pLaunchArguments->setMinimumHeight(LAUNCH_ROW_HEIGHT);
    pLaunchArguments->setMaximumHeight(LAUNCH_ROW_HEIGHT);
    ui_->launchTree->setItemWidget(pLauncherCMD, 1, pLaunchArguments);
    connect(pLaunchArguments, SIGNAL(textEdited(const QString &)), this, SLOT(launchArgsEdited(const QString &)));

    // Comming soon
    //    pButton = new QPushButton();
    //    pButton->setText("...");
    //    ui->launchTree->setItemWidget(pLauncherCMD, 2, pButton);

    //////////////////////////////////////////////////////////////////
    // LOG FILE
    QTreeWidgetItem *pLauncherLogFile = new QTreeWidgetItem();
    pLauncherLogFile->setText(0, "Output Log");
    pLauncherParent->addChild(pLauncherLogFile);

    pLaunchLogFileEdit = new QLineEdit();
    pLaunchLogFileEdit->setMinimumHeight(LAUNCH_ROW_HEIGHT);
    pLaunchLogFileEdit->setMaximumHeight(LAUNCH_ROW_HEIGHT);
    ui_->launchTree->setItemWidget(pLauncherLogFile, 1, pLaunchLogFileEdit);

    pLaunchLogFilebutton = new QPushButton();
    pLaunchLogFilebutton->setText("...");
    pLaunchLogFilebutton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    pLaunchLogFilebutton->setMaximumWidth(LAUNCH_COLUMN2_SIZE);
    ui_->launchTree->setItemWidget(pLauncherLogFile, 2, pLaunchLogFilebutton);
    connect(pLaunchLogFilebutton, SIGNAL(clicked()), this, SLOT(launchSetLogFile()));

    //////////////////////////////////////////////////////////////////
    ui_->launchTree->setMinimumHeight(LAUNCH_ROW_HEIGHT * 4 + 6);
    ui_->launchTree->setMaximumHeight(LAUNCH_ROW_HEIGHT * 4 + 6);

    ui_->launchTree->setColumnWidth(0, LAUNCH_COLUMN0_SIZE);
    ui_->launchTree->setColumnWidth(
        1, ui_->launchTree->rect().width() - LAUNCH_COLUMN0_SIZE - LAUNCH_COLUMN2_SIZE - LAUNCH_SPACING_SIZE);
    ui_->launchTree->setColumnWidth(2, LAUNCH_COLUMN2_SIZE);

    ui_->launchTree->expandItem(pLauncherParent);
    ui_->launchTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui_->launchTree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    ResetLaunchOptions();
}

////////////////////////////////////////////////////////////////////
// Expanding the tree also grows the tree to match
void MainWindow::launchItemExpanded(QTreeWidgetItem *item) {
    (void)item;
    ui_->launchTree->setMinimumHeight(LAUNCH_ROW_HEIGHT * 4 + 6);
    ui_->launchTree->setMaximumHeight(LAUNCH_ROW_HEIGHT * 4 + 6);
}

////////////////////////////////////////////////////////////////////
// Collapsing the tree also shrinks the tree to match and show only
// the first line
void MainWindow::launchItemCollapsed(QTreeWidgetItem *item) {
    QRect rect = ui_->launchTree->visualItemRect(item);
    ui_->launchTree->setMinimumHeight(rect.height());
    ui_->launchTree->setMaximumHeight(rect.height());
}

////////////////////////////////////////////////////////////////////
void MainWindow::launchSetLogFile() {
    int nLaunchIndex = pLaunchAppsCombo->currentIndex();
    Q_ASSERT(nLaunchIndex >= 0);

    QString logFile = QFileDialog::getSaveFileName(this, tr("Set Log File To..."), ".", tr("Log text(*.txt)"));

    logFile = QDir::toNativeSeparators(logFile);

    Configurator &configurator = Configurator::Get();
    configurator.overridden_application_list[nLaunchIndex]->log_file = logFile;

    if (logFile.isEmpty())
        pLaunchLogFileEdit->setText("");
    else
        pLaunchLogFileEdit->setText(logFile);

    configurator.SaveOverriddenApplicationList();
}

////////////////////////////////////////////////////////////////////
/// Launch app change
void MainWindow::launchItemChanged(int application_index) {
    Configurator &configurator = Configurator::Get();

    if (application_index >= configurator.overridden_application_list.size()) return;

    if (application_index < 0) return;

    pLaunchArguments->setText(configurator.overridden_application_list[application_index]->arguments);
    pLaunchWorkingFolder->setText(configurator.overridden_application_list[application_index]->working_folder);
    pLaunchLogFileEdit->setText(configurator.overridden_application_list[application_index]->log_file);

    configurator.SelectLaunchApplication(application_index);
    configurator.SaveSettings();
}

/////////////////////////////////////////////////////////////////////
/// \brief MainWindow::launchArgsEdited
/// \param newText
/// New command line arguments. Update them.
void MainWindow::launchArgsEdited(const QString &arguments) {
    int application_index = pLaunchAppsCombo->currentIndex();
    if (application_index < 0) return;

    Configurator &configurator = Configurator::Get();
    configurator.overridden_application_list[application_index]->arguments = arguments;
    configurator.SaveOverriddenApplicationList();
}

//////////////////////////////////////////////////////////////////////
// Clear the browser window
void MainWindow::on_pushButtonClearLog_clicked() {
    ui_->logBrowser->clear();
    ui_->logBrowser->update();
    ui_->pushButtonClearLog->setEnabled(false);
}

//////////////////////////////////////////////////////////////////////
bool MainWindow::eventFilter(QObject *target, QEvent *event) {
    // Launch tree does some fancy resizing and since it's down in
    // layouts and splitters, we can't just relay on the resize method
    // of this window.
    if (target == ui_->launchTree) {
        if (event->type() == QEvent::Resize) {
            QRect rect = ui_->launchTree->rect();
            ui_->launchTree->setColumnWidth(0, LAUNCH_COLUMN0_SIZE);
            ui_->launchTree->setColumnWidth(1, rect.width() - LAUNCH_COLUMN0_SIZE - LAUNCH_COLUMN2_SIZE - LAUNCH_SPACING_SIZE);
            ui_->launchTree->setColumnWidth(2, LAUNCH_COLUMN2_SIZE);
            return false;
        }
    }

    // Context menus for layer configuration files
    if (target == ui_->profileTree) {
        QContextMenuEvent *right_click = dynamic_cast<QContextMenuEvent *>(event);
        if (right_click && event->type() == QEvent::ContextMenu) {
            // Which item were we over?
            QTreeWidgetItem *configuration_item = ui_->profileTree->itemAt(right_click->pos());
            ContigurationListItem *item = dynamic_cast<ContigurationListItem *>(configuration_item);

            // Create context menu here
            QMenu menu(ui_->profileTree);

            QAction *pNewAction = new QAction("New Layers Configuration...");
            pNewAction->setEnabled(true);
            menu.addAction(pNewAction);

            QAction *pDuplicateAction = new QAction("Duplicate the Layers Configuration");
            pDuplicateAction->setEnabled(item != nullptr);
            menu.addAction(pDuplicateAction);

            QAction *pRemoveAction = new QAction("Remove the Layers Configuration");
            pRemoveAction->setEnabled(item != nullptr);
            menu.addAction(pRemoveAction);

            QAction *pRenameAction = new QAction("Rename the Layers Configuration");
            pRenameAction->setEnabled(item != nullptr);
            menu.addAction(pRenameAction);

            menu.addSeparator();

            QAction *pImportAction = new QAction("Import a Layers Configuration...");
            pImportAction->setEnabled(true);
            menu.addAction(pImportAction);

            QAction *pExportAction = new QAction("Export the Layers Configuration...");
            pExportAction->setEnabled(item != nullptr);
            menu.addAction(pExportAction);

            menu.addSeparator();

            QAction *pCustomPathAction = new QAction("Edit Layers Custom Path...");
            pCustomPathAction->setEnabled(true);
            menu.addAction(pCustomPathAction);

            QPoint point(right_click->globalX(), right_click->globalY());
            QAction *action = menu.exec(point);

            // Pointer compares made me throw up in my mouth at least a little
            // less than doing a full string compare. Setting up signal/slot for
            // all of these just seemed ridiculous. Every problem is not a nail,
            // put the hammer away....
            // New Profile...
            if (action == pNewAction) {
                settings_tree_manager_.CleanupGUI();
                NewClicked();
                ui_->groupBoxEditor->setTitle(tr(EDITOR_CAPTION_EMPTY));
                return true;
            }

            // Duplicate
            if (action == pDuplicateAction) {
                settings_tree_manager_.CleanupGUI();
                DuplicateClicked(item);
                settings_tree_manager_.CleanupGUI();
                ui_->groupBoxEditor->setTitle(tr(EDITOR_CAPTION_EMPTY));
                return true;
            }

            // Remove this profile....
            if (action == pRemoveAction) {
                settings_tree_manager_.CleanupGUI();
                RemoveClicked(item);
                ui_->groupBoxEditor->setTitle(tr(EDITOR_CAPTION_EMPTY));
                return true;
            }

            // Rename this profile...
            if (action == pRenameAction) {
                RenameClicked(item);
                settings_tree_manager_.CleanupGUI();
                ui_->groupBoxEditor->setTitle(tr(EDITOR_CAPTION_EMPTY));
                return true;
            }

            // Export this profile (copy the .json)
            if (action == pExportAction) {
                settings_tree_manager_.CleanupGUI();
                ExportClicked(item);
                ui_->groupBoxEditor->setTitle(tr(EDITOR_CAPTION_EMPTY));
                return true;
            }

            // Import a profile (copy a json)
            if (action == pImportAction) {
                settings_tree_manager_.CleanupGUI();
                ImportClicked(item);
                ui_->groupBoxEditor->setTitle(tr(EDITOR_CAPTION_EMPTY));
                return true;
            }

            // Edit Layer custom paths
            if (action == pCustomPathAction) {
                settings_tree_manager_.CleanupGUI();
                EditCustomPathsClicked(item);
                ui_->groupBoxEditor->setTitle(tr(EDITOR_CAPTION_EMPTY));
                return true;
            }

            // Do not pass on
            return true;
        }
    }

    // Pass it on
    return false;
}

///////////////////////////////////////////////////////////////////////////////////
/// Launch the app and monitor it's stdout to get layer output.
/// stdout is buffered by default, so in order to see realtime output it must
/// be flushed. Either of the following in the other app will do.
/// > fflush(stdout);    // Flush now
/// setlinebuf(stdout);  // always flush at the end of a line
///
/// The layers are automtically flushed, so they should show up as they
/// generated. One note... any other stdout generated by the monitored
/// application will also be captured.
///
/// If logging is enbabled (by setting a logging file), then the log file
/// is also opened.
void MainWindow::on_pushButtonLaunch_clicked() {
    // Are we already monitoring a running app? If so, terminate it
    if (launch_application_ != nullptr) {
        launch_application_->terminate();
        launch_application_->deleteLater();
        launch_application_ = nullptr;
        ui_->pushButtonLaunch->setText(tr("Launch"));
        return;
    }

    // Is there an app selected?
    int nIndex = pLaunchAppsCombo->currentIndex();

    // Launch the test application
    launch_application_ = new QProcess(this);
    connect(launch_application_, SIGNAL(readyReadStandardOutput()), this, SLOT(standardOutputAvailable()));

    connect(launch_application_, SIGNAL(readyReadStandardError()), this, SLOT(errorOutputAvailable()));

    connect(launch_application_, SIGNAL(finished(int, QProcess::ExitStatus)), this, SLOT(processClosed(int, QProcess::ExitStatus)));

    Configurator &configurator = Configurator::Get();
    launch_application_->setProgram(configurator.overridden_application_list[nIndex]->executable_path);
    launch_application_->setWorkingDirectory(configurator.overridden_application_list[nIndex]->working_folder);

    if (!configurator.overridden_application_list[nIndex]->arguments.isEmpty()) {
        QStringList args = configurator.overridden_application_list[nIndex]->arguments.split(" ");
        launch_application_->setArguments(args);
    }

    // Some of these may have changed
    configurator.SaveSettings();

    launch_application_->start(QIODevice::ReadOnly | QIODevice::Unbuffered);
    launch_application_->setProcessChannelMode(QProcess::MergedChannels);
    launch_application_->closeWriteChannel();

    // We are logging, let's add that we've launched a new application
    QString launchLog = "Launching Vulkan Application:\n";

    if (configurator.GetActiveConfiguration() == nullptr) {
        launchLog += QString().asprintf("- Layers fully controlled by the application.\n");
    } else if (!configurator.GetActiveConfiguration()->IsValid()) {
        launchLog += QString().asprintf("- No layers override. The active \"%s\" configuration is missing a layer.\n",
                                        configurator.GetActiveConfiguration()->name.toUtf8().constData());
    } else if (configurator.override_active) {
        launchLog += QString().asprintf("- Layers overridden by \"%s\" configuration.\n",
                                        configurator.GetActiveConfiguration()->name.toUtf8().constData());
    }

    launchLog += QString().asprintf("- Executable Path: %s\n",
                                    configurator.overridden_application_list[nIndex]->executable_path.toUtf8().constData());
    launchLog += QString().asprintf("- Working Directory: %s\n",
                                    configurator.overridden_application_list[nIndex]->working_folder.toUtf8().constData());
    launchLog += QString().asprintf("- Command-line Arguments: %s\n",
                                    configurator.overridden_application_list[nIndex]->arguments.toUtf8().constData());

    if (!configurator.overridden_application_list[nIndex]->log_file.isEmpty()) {
        // This should never happen... but things that should never happen do in
        // fact happen... so just a sanity check.
        if (log_file_ != nullptr) {
            log_file_->close();
            log_file_ = nullptr;
        }

        // Start logging
        log_file_ = new QFile(configurator.overridden_application_list[nIndex]->log_file);

        // Open and append, or open and truncate?
        QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text;
        if (!ui_->checkBoxClearOnLaunch->isChecked()) mode |= QIODevice::Append;

        if (!configurator.overridden_application_list[nIndex]->log_file.isEmpty()) {
            if (!log_file_->open(mode)) {
                QMessageBox err;
                err.setText(tr("Cannot open log file"));
                err.setIcon(QMessageBox::Warning);
                err.exec();
                delete log_file_;
                log_file_ = nullptr;
            }
        }

        if (log_file_) {
            log_file_->write((launchLog + "\n").toUtf8().constData(), launchLog.length());
        }
    }

    if (ui_->checkBoxClearOnLaunch->isChecked()) ui_->logBrowser->clear();
    ui_->logBrowser->append(launchLog);
    ui_->pushButtonClearLog->setEnabled(true);

    // Wait... did we start? Give it 4 seconds, more than enough time
    if (!launch_application_->waitForStarted(4000)) {
        launch_application_->waitForStarted();
        launch_application_->deleteLater();
        launch_application_ = nullptr;

        QString outFailed = QString().asprintf(
            "Failed to launch %s!\n", configurator.overridden_application_list[nIndex]->executable_path.toUtf8().constData());

        ui_->logBrowser->append(outFailed);
        if (log_file_) log_file_->write(outFailed.toUtf8().constData(), outFailed.length());

        return;
    }

    // We are off to the races....
    ui_->pushButtonLaunch->setText(tr("Terminate"));
}

/////////////////////////////////////////////////////////////////////////////
/// The process we are following is closed. We don't actually care about the
/// exit status/code, we just need to know to destroy the QProcess object
/// and set it back to nullptr so that we know we can launch a new app.
/// Also, if we are logging, it's time to close the log file.
void MainWindow::processClosed(int exit_code, QProcess::ExitStatus status) {
    (void)exit_code;
    (void)status;

    // Not likely, but better to be sure...
    if (launch_application_ == nullptr) return;

    disconnect(launch_application_, SIGNAL(finished(int, QProcess::ExitStatus)), this,
               SLOT(processClosed(int, QProcess::ExitStatus)));

    disconnect(launch_application_, SIGNAL(readyReadStandardError()), this, SLOT(errorOutputAvailable()));

    disconnect(launch_application_, SIGNAL(readyReadStandardOutput()), this, SLOT(standardOutputAvailable()));

    ui_->pushButtonLaunch->setText(tr("Launch"));

    if (log_file_) {
        log_file_->close();
        delete log_file_;
        log_file_ = nullptr;
    }

    delete launch_application_;
    launch_application_ = nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// This signal get's raised whenever the spawned Vulkan appliction writes
/// to stdout and there is data to be read. The layers flush after all stdout
/// writes, so we should see layer output here in realtime, as we just read
/// the string and append it to the text browser.
/// If a log file is open, we also write the output to the log.
void MainWindow::standardOutputAvailable() {
    if (launch_application_ == nullptr) return;

    QString inFromApp = launch_application_->readAllStandardOutput();
    ui_->logBrowser->append(inFromApp);
    ui_->pushButtonClearLog->setEnabled(true);

    // Are we logging?
    if (log_file_) log_file_->write(inFromApp.toUtf8().constData(), inFromApp.length());
}

///////////////////////////////////////////////////////////////////////////////
void MainWindow::errorOutputAvailable() {
    if (launch_application_ == nullptr) return;

    QString inFromApp = launch_application_->readAllStandardError();
    ui_->logBrowser->append(inFromApp);
    ui_->pushButtonClearLog->setEnabled(true);

    // Are we logging?
    if (log_file_) log_file_->write(inFromApp.toUtf8().constData(), inFromApp.length());
}
