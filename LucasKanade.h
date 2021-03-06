﻿#pragma once

#include <QGroupBox>
#include <QPointer>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <biotracker/TrackingAlgorithm.h>
#include <biotracker/util/MutexWrapper.h>

#include <opencv2/video/tracking.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <ctype.h>

#include "InterestPoint.h"

/*
 * Inspired by:
 * https://github.com/Itseez/opencv/blob/master/samples/cpp/lkdemo.cpp
 */
class LucasKanadeTracker : public BioTracker::Core::TrackingAlgorithm {
    Q_OBJECT
  public:
    LucasKanadeTracker(BioTracker::Core::Settings &settings);

    void track(size_t frameNumber, const cv::Mat &frame) override;
    void paint(size_t frameNumber, BioTracker::Core::ProxyMat &m, View const &view = OriginalView) override;
    void paintOverlay(size_t frameNumber, QPainter *painter, View const &view = OriginalView) override;

    std::set<Qt::Key> const &grabbedKeys() const override {
        return m_grabbedKeys;
    }

    void keyPressEvent(QKeyEvent *ev) override;

  private:
    // --
    bool				m_isInitialized = false;
    size_t				m_numberOfUserStates = 3;
    std::vector<bool>	m_setUserStates;

    int					m_itemSize; // defines how big elements are (so they fit well on big and small vids)
    cv::Size			m_subPixWinSize;
    cv::Size			m_winSize;
    cv::TermCriteria	m_termcrit;
    const int			MAX_COUNT = 500;
    cv::Mat				m_gray;

	size_t				m_frameIndex_prevGray; // holds the video index that corresponds to m_prevGray
    cv::Mat				m_prevGray;		

    size_t				m_currentFrame; // is always the current frame (updated in paint and track)

    bool				m_trackOnlyActive; // when true we will ignore all points except the active one
    bool				m_pauseOnInvalidPoint; // if true, the application will pause when a point
                            // becomes invalid


	// as we want to adapt the values of this class all the time we need to
	// keep it accessible from other methods in the object..
    QSlider	*			m_winSizeSlider;                       
    QLabel	*			m_winSizeValue;
    QSlider *			m_historySlider; // define how many elements are shown for history
    QLabel	*			m_historyValue;

    std::set<Qt::Key>	m_grabbedKeys;

    /**
     * @brief m_invalidOffset
     */
    const cv::Point2f m_invalidOffset;

    /**
     * @brief m_currentActivePoint
     * The currently active point that can be moved by the mouse curor
     */
    int m_currentActivePoint = -1;
    int m_lastDrawnActivePointX = -1;
    int m_lastDrawnActivePointY = -1;

    // to calculate how big the history can be we need to know when we had the very first tracked point in time...
    int m_firstTrackedFrame = -1;

    // ... and also the very last
    int m_lastTrackedFrame = -1;

    size_t m_currentHistory = 0;

    QColor m_validColor;
    QColor m_invalidColor;

    Mutex m_userStatusMutex;
    // --

    void mouseReleaseEvent(QMouseEvent *e) override;

    void inputChanged() override;

    /**
     * @brief createNewPoint
     * Tries to add a new point, if it is not too close to an already
     * existing one
     * @param pos
     */
    void tryCreateNewPoint(QPoint pos);


    void activateExistingPoint(QPoint pos);
    void moveCurrentActivePointTo(QPoint pos);

    void deleteCurrentActivePoint();

    /**
     * @brief autoFindInitPoints
     * call this when you want to find let the program find interesting
     * points
     */
    void autoFindInitPoints();

    /**
     * @brief getCurrentPoints
     * gets the locations of all points at the given timeframe
     * @param frameNbr defines the current track-iteratioyn
     * @param filter: marks those indices that point to an invalid
     * 	(for whatever reason) trajectory => OUT-parameter
     *  The index represents the id of the trajectory data
     * @return the list of points with positions
     */
    std::vector<cv::Point2f> getCurrentPoints(
        ulong frameNbr,
        std::vector<InterestPointStatus> &filter,
        std::vector<InterestPoint> &data);

    /**
     * @brief updateCurrentPoints
     * update the current locations to the serializable trajectory data
     * for the current frame
     * @param frameNbr defines the current track-iteration
     * @param pos list of updated positions, the index equals the
     * the element id
     * @param status defines the list of object status` that happened to each
     * object
     * @param filter: marks those indices that point to an invalid
     * 	(for whatever reason) trajectory
     *  The index represents the id of the trajectory data
     */
    void updateCurrentPoints(
        ulong frameNbr,
        std::vector<cv::Point2f> &pos,
        std::vector<uchar> &status,
        std::vector<InterestPointStatus> &filter);

    cv::Point2f toCv(QPoint p);

    int maximumHistory();

    void updateHistoryText();

    /**
     * @brief clampPosition
     * make sure that the elements are in the range of the image
     * @param pos list of positions
     * @param w current image width
     * @param h current image height
     */
    void clampPosition(std::vector<cv::Point2f> &pos, int w, int h);

    /**
     * @brief splitActivePoints
     * Due to how this tracker is written we get all points for tracking with the
     * OpenCV LK function. However, it would be great to filter out those that we should
     * not follow anymore (as they might be invalid, deleted or whatever): thus this
     * function temporarly cleans the points. To restitch the points later on to update
     * the internal data structures correctly "joinActivePoints" MUST be called with the
     * data aquired here to fix indexes.
     * @param pos The points aquired from "getCurrentPoints" that should be filtered
     * @param filter The filters aquired from "getCurrentPoints"
     * @param tempPos OUT: points from "pos" that are acutally trackable
     * @param activePoints: indexes of all points in tempPos
     */
    void splitActivePoints(std::vector<cv::Point2f> &pos, std::vector<InterestPointStatus> &filter,
                           std::vector<cv::Point2f> &tempPos, std::vector<size_t> &activePoints);

    /**
     * @brief joinActivePoints
     * Fix indexes "broken" by "splitActivePoints"
     * @param pos All points available, this list should be updated with the active points
     * @param tempPos Actual points that were used for the OpenCV LK tracking
     * @param activePoints indexes of the tempPos in the pos list
     * @param status list aquired from the OpenCV LK tracker function
     * @return actual status list that should be used in "updateCurrentPoints"
     */
    std::vector<uchar> joinActivePoints(std::vector<cv::Point2f> &pos, std::vector<cv::Point2f> &tempPos,
                          std::vector<size_t> &activePoints, std::vector<uchar> status);

    /**
     * @brief activateAllNonTrackedPoints
     * When single-user-tracking is disabled, we want to activate all points that were
     * deactiaveted
     * @param frame
     */
    void activateAllNonTrackedPoints(size_t frame);

    /**
     * @brief updateUserStates
     * make sure that all the user states are updated
     */
    void updateUserStates(size_t currentFrame);

    void drawEllipse(QPainter* painter, QPen& pen, InterestPoint &point, size_t id, int x, int y);

private Q_SLOTS:
    void checkboxChanged_invalidPoint(int state);
    void checkboxChanged_userStatus(int state);
    void checkboxChanged_activeUser(int state);
    void clicked_validColor();
    void clicked_invalidColor();
    void clicked_print();
    void colorSelected_invalid(const QColor &color);
    void colorSelected_valid(const QColor &color);
    void sliderChanged_winSize(int value);
    void sliderChanged_history(int value);

};
