// SPDX-License-Identifier: GPL-2.0
/*
 * maintab.h
 *
 * header file for the main tab of Subsurface
 *
 */
#ifndef MAINTAB_H
#define MAINTAB_H

#include <QTabWidget>
#include <QDialog>
#include <QMap>
#include <QUuid>

#include "ui_maintab.h"
#include "qt-models/completionmodels.h"
#include "qt-models/divelocationmodel.h"
#include "core/dive.h"
#include "core/subsurface-qt/DiveListNotifier.h"

class ExtraDataModel;
class DivePictureModel;

class TabBase;
class MainTab : public QTabWidget {
	Q_OBJECT
public:
	enum EditMode {
		NONE,
		DIVE,
		MANUALLY_ADDED_DIVE,
		IGNORE
	};

	MainTab(QWidget *parent = 0);
	~MainTab();
	void clearTabs();
	void reload();
	void initialUiSetup();
	bool isEditing();
	void updateCoordinatesText(qreal lat, qreal lon);
	void refreshDisplayedDiveSite();
	void nextInputField(QKeyEvent *event);
	void stealFocus();

signals:
	void diveSiteChanged();
public
slots:
	void divesChanged(dive_trip *trip, const QVector<dive *> &dives, DiveField field);
	void diveSiteEdited(dive_site *ds, int field);
	void tripChanged(dive_trip *trip, TripField field);
	void updateDiveInfo();
	void updateNotes(const struct dive *d);
	void updateMode(struct dive *d);
	void updateDateTime(struct dive *d);
	void updateDiveSite(struct dive *d);
	void acceptChanges();
	void rejectChanges();
	void on_location_diveSiteSelected();
	void on_locationPopupButton_clicked();
	void on_divemaster_editingFinished();
	void on_buddy_editingFinished();
	void on_suit_editingFinished();
	void on_diveTripLocation_editingFinished();
	void on_notes_editingFinished();
	void on_airtemp_editingFinished();
	void on_duration_editingFinished();
	void on_depth_editingFinished();
	void divetype_Changed(int);
	void on_watertemp_editingFinished();
	void on_dateEdit_dateChanged(const QDate &date);
	void on_timeEdit_timeChanged(const QTime & time);
	void on_rating_valueChanged(int value);
	void on_visibility_valueChanged(int value);
	void on_tagWidget_editingFinished();
	void addMessageAction(QAction *action);
	void hideMessage();
	void closeMessage();
	void displayMessage(QString str);
	void enableEdition(EditMode newEditMode = NONE);
	void updateTextLabels(bool showUnits = true);
	void escDetected(void);
private:
	Ui::MainTab ui;
	EditMode editMode;
	BuddyCompletionModel buddyModel;
	DiveMasterCompletionModel diveMasterModel;
	SuitCompletionModel suitModel;
	TagCompletionModel tagModel;
	bool modified;
	bool lastSelectedDive;
	int lastTabSelectedDive;
	int lastTabSelectedDiveTrip;
	dive_trip_t *currentTrip;
	QList<TabBase*> extraWidgets;
};

#endif // MAINTAB_H
