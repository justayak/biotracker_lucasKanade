#include "LucasKanade.h"

#include <QApplication>
#include <QIntValidator>
#include <QPushButton>
#include <QPainter>
#include <QColorDialog>
#include <QDateTime>

#include <QFileDialog>
#include <biotracker/TrackingAlgorithm.h>
#include <biotracker/Registry.h>

using namespace BioTracker::Core;

extern "C" {
    #ifdef _WIN32
    void __declspec(dllexport) registerTracker() {
    #else
    void registerTracker() {
    #endif
        BioTracker::Core::Registry::getInstance().registerTrackerType<LucasKanadeTracker>("Lucas-Kanade");
    }
}

LucasKanadeTracker::LucasKanadeTracker(Settings &settings):
    TrackingAlgorithm(settings),
    m_setUserStates(m_numberOfUserStates),
    m_itemSize(1),
    m_subPixWinSize(10, 10),
    m_winSize(31, 31),
    m_termcrit(cv::TermCriteria::COUNT | cv::TermCriteria::EPS,20,0.03),
    m_trackOnlyActive(false),
    m_pauseOnInvalidPoint(false),
    m_winSizeSlider(new QSlider(getToolsWidget())),
    m_winSizeValue(new QLabel(QString::number(m_winSize.height), getToolsWidget())),
    m_historySlider(new QSlider(getToolsWidget())),
    m_historyValue(new QLabel("0", getToolsWidget())),
    m_invalidOffset(-99999, -99999),
    m_validColor(QColor::fromRgb(0, 0, 255)),
    m_invalidColor(QColor::fromRgb(255, 0, 0))

{
	m_grabbedKeys.insert(Qt::Key_D);

    // initialize gui
    auto ui = getToolsWidget();
    auto layout = new QGridLayout();

    // User status
    for (size_t i = 0; i < m_numberOfUserStates; i++) {
        auto text = QString("Status ");
        text.append(QString::number(i+1));
        auto *chkboxUserStatus = new QCheckBox(text, ui);
        chkboxUserStatus->setAccessibleName(QString::number(i)); // hack to re-identify the checkbox
        QObject::connect(chkboxUserStatus, &QCheckBox::stateChanged,
            this, &LucasKanadeTracker::checkboxChanged_userStatus);
        layout->addWidget(chkboxUserStatus, 10, static_cast<int>(i), 1, 1);
    }


    // Checkbox for pausing on invalid points
    auto *chkboxInvalidPoints = new QCheckBox("Pause on invalid Point", ui);
    chkboxInvalidPoints->setChecked(false);
    QObject::connect(chkboxInvalidPoints, &QCheckBox::stateChanged,
        this, &LucasKanadeTracker::checkboxChanged_invalidPoint);
    layout->addWidget(chkboxInvalidPoints, 1, 0, 1, 3);

    // Checkbox for tracking only the current active point
    auto *chkboxActivePoints = new QCheckBox("Track only active point", ui);
    chkboxActivePoints->setChecked(false);
    QObject::connect(chkboxActivePoints, &QCheckBox::stateChanged,
        this, &LucasKanadeTracker::checkboxChanged_activeUser);
    layout->addWidget(chkboxActivePoints, 2, 0, 1, 3);

    // history
    auto *lbl_history = new QLabel("history", ui);
    m_historySlider->setMinimum(0);
    m_historySlider->setMaximum(maximumHistory());
    m_historySlider->setOrientation(Qt::Orientation::Horizontal);
    layout->addWidget(lbl_history, 3, 0, 1, 1);
    layout->addWidget(m_historyValue, 3, 2, 1, 1);
    layout->addWidget(m_historySlider, 4, 0, 1, 3);
    QObject::connect(m_historySlider, &QSlider::valueChanged,
        this, &LucasKanadeTracker::sliderChanged_history);

    // winsize
    auto *lbl_winSize = new QLabel("window size:", ui);
    m_winSizeSlider->setMinimum(10);
    m_winSizeSlider->setMaximum(m_winSize.height);
    m_winSizeSlider->setOrientation(Qt::Orientation::Horizontal);
    m_winSizeSlider->setValue(m_winSize.height);
    QObject::connect(m_winSizeSlider, &QSlider::valueChanged,
        this, &LucasKanadeTracker::sliderChanged_winSize);
    layout->addWidget(lbl_winSize, 5, 0, 1, 1);
    layout->addWidget(m_winSizeValue, 6, 2, 1, 1);
    layout->addWidget(m_winSizeSlider, 6, 0, 1, 2);

    // colors
    auto lbl_color = new QLabel("Change color:", ui);
    layout->addWidget(lbl_color, 7, 0, 1, 1);

    auto validColorBtn = new QPushButton("Valid color", ui);
    QObject::connect(validColorBtn, &QPushButton::clicked,
        this, &LucasKanadeTracker::clicked_validColor);
    layout->addWidget(validColorBtn, 7, 1, 1, 1);

    auto invalidColorBtn = new QPushButton("Invalid color", ui);
    QObject::connect(invalidColorBtn, &QPushButton::clicked,
        this, &LucasKanadeTracker::clicked_invalidColor);
    layout->addWidget(invalidColorBtn, 8, 1, 1, 1);

    // print
    auto printBtn = new QPushButton("Export", ui);
    QObject::connect(printBtn, &QPushButton::clicked,
        this, &LucasKanadeTracker::clicked_print);
    layout->addWidget(printBtn, 9, 0, 1, 1);

    // ===

    ui->setLayout(layout);
}

void LucasKanadeTracker::track(size_t frame, const cv::Mat &imgOriginal) {
    m_userStatusMutex.Lock();
    // Landscape vs	portrait
    // [xxxx]		[xx]
    // [xxxx]		[xx]
    //       		[xx]
    //
    const bool isLandscape = imgOriginal.rows > imgOriginal.cols;

	// make the winSize adaptable
    const int currentMaxWinSize = m_winSizeSlider->maximum();
    const int newMaxWinSize = isLandscape ? imgOriginal.cols / 10 : imgOriginal.rows / 10;
    if (currentMaxWinSize != newMaxWinSize && newMaxWinSize > m_winSizeSlider->minimum()) {
        m_winSizeSlider->setMaximum(newMaxWinSize);
    }

    m_currentFrame = frame; // TODO must this be protected from other threads?
    cv::cvtColor(imgOriginal, m_gray, cv::COLOR_BGR2GRAY);

    std::vector<InterestPointStatus> filter;
    std::vector<InterestPoint> data;
    std::vector<cv::Point2f> currentPoints = getCurrentPoints(static_cast<ulong>(frame) - 1, filter, data);

    if (m_prevGray.empty()) {
        m_gray.copyTo(m_prevGray);
            m_frameIndex_prevGray = m_currentFrame;
    }

    // clamp away invalid points:
    std::vector<cv::Point2f> currentPointsOnlyActive;
    std::vector<size_t> activePointIds;
    splitActivePoints(currentPoints,
              filter,
              currentPointsOnlyActive,
              activePointIds);

    if (!currentPointsOnlyActive.empty()) {
        std::vector<float> err;

        // calculate pyramids:
        const size_t maxLevel = 10;
        std::vector<cv::Mat> prevPyr;
        cv::buildOpticalFlowPyramid(m_prevGray, prevPyr, m_winSize, maxLevel);

        std::vector<cv::Mat> pyr;
        cv::buildOpticalFlowPyramid(m_gray, pyr, m_winSize, maxLevel);

        std::vector<cv::Point2f> newPoints;
        std::vector<uchar> status;
        cv::calcOpticalFlowPyrLK(
        prevPyr, /* prev */
        pyr, /* next */
        currentPointsOnlyActive,	/* prevPts */
        newPoints, /* nextPts */
        status,	/* status */
        err	/* err */
        ,m_winSize,	/* winSize */
        maxLevel, /* maxLevel */
        m_termcrit,	/* criteria */
        0, /* flags */
        0.001 /* minEigThreshold */
        );

        // put together the clamped away points
        status = joinActivePoints(currentPoints,
                                  newPoints,
                                  activePointIds,
                                  status);

        clampPosition(newPoints, m_gray.cols, m_gray.rows);
        updateCurrentPoints(static_cast<ulong>(frame), currentPoints, status, filter);
        updateHistoryText();
        updateUserStates(frame);
    }

    cv::swap(m_prevGray, m_gray);
    m_frameIndex_prevGray = m_currentFrame;

    m_userStatusMutex.Unlock();
}

void LucasKanadeTracker::paint(size_t, ProxyMat & mat, const TrackingAlgorithm::View &) {
	// when frames are skipped without tracking we have outdated gray frames yielding tracking errors
    m_userStatusMutex.Lock();
    if (!isTrackingActivated() && ( m_currentFrame != m_frameIndex_prevGray )) {
		cv::cvtColor(mat.getMat(), m_prevGray, cv::COLOR_BGR2GRAY);
		m_frameIndex_prevGray = m_currentFrame; // all consecutive calls are thus not copying the frame any more
    }

    if (!m_isInitialized) {
		cv::cvtColor(mat.getMat(), m_gray, cv::COLOR_BGR2GRAY);

		const bool isLandscape = mat.getMat().rows > mat.getMat().cols;

		// make sure that the circles are in "good" size, regardless of the video resolution (tiny vs gigantic)
		const int perc_size = 45;
		m_itemSize = !isLandscape ? mat.getMat().rows / perc_size : mat.getMat().cols / perc_size;

	}

    m_userStatusMutex.Unlock();
}

void LucasKanadeTracker::paintOverlay(size_t currentFrame, QPainter *painter, const View &) {
    m_userStatusMutex.Lock();
    // update current frame counter in case the track function is disabled.
	// not-so-nice solution since the same line appears in function "track"
	m_currentFrame = currentFrame; // TODO must this be protected from other threads?
	
	std::vector<InterestPointStatus> filter;
    std::vector<InterestPoint> data;
    std::vector<cv::Point2f> newPoints = getCurrentPoints(static_cast<ulong>(currentFrame), filter, data);

    // fill the history
    std::vector<std::vector<cv::Point2f>> history;
    for (size_t t = 1; t < m_currentHistory; t++) {
        if (t > currentFrame) break;
        ulong histTime = static_cast<ulong>(currentFrame - t);
        
        std::vector<InterestPointStatus> dummyfilter;
        std::vector<InterestPoint> data2;
        std::vector<cv::Point2f> histPoints = getCurrentPoints(histTime, dummyfilter, data2);
        history.push_back(histPoints);
    }

    bool currentActivePointIsDrawn = false;
    QFont font = painter->font();
    font.setPixelSize(m_itemSize);
    painter->setFont(font);
    size_t i;
    for (i = 0; i < newPoints.size(); i++) {
        if (filter[i] == InterestPointStatus::Non_Existing) {
            // the i`th point became invalid
            continue;
        }

        QColor color = m_validColor;
        auto point = newPoints[i];
        if (filter[i] == InterestPointStatus::Invalid) {
            point -= m_invalidOffset;
            color = m_invalidColor;
        } else if (filter[i] == InterestPointStatus::Not_Tracked) {
            color.setAlpha(100);
        }

        int x = static_cast<int>(point.x);
        int y = static_cast<int>(point.y);

        QPen p(color);
        if (i == static_cast<size_t>(m_currentActivePoint)) {
            p.setStyle(Qt::PenStyle::DotLine);
            m_lastDrawnActivePointX = x;
            m_lastDrawnActivePointY = y;
            currentActivePointIsDrawn = true;
        }

        drawEllipse(painter, p, data[i], i, x, y);

        // paint History
        color.setAlpha(100);
        QPen histPen(color);
        painter->setPen(histPen);
        for (auto histPoints : history) {
            auto histPoint = histPoints[i];
            int x = static_cast<int>(histPoint.x);
            int y = static_cast<int>(histPoint.y);
            if (x > 0 && y > 0) { // otherwise the point is invalid
                painter->drawRect(x, y, 1, 1);
            }
        }
    }

    if (!currentActivePointIsDrawn && m_currentActivePoint >= 0) {
        // When tracking is deactivated we want to see at least where the currently activated
        // point was last..
        QColor color = m_validColor;
        color.setAlpha(100);
        QPen p(color);
        p.setStyle(Qt::PenStyle::DotLine);
        drawEllipse(painter, p, data[m_currentActivePoint], m_currentActivePoint, m_lastDrawnActivePointX, m_lastDrawnActivePointY);
    }

    m_userStatusMutex.Unlock();
}

void LucasKanadeTracker::keyPressEvent(QKeyEvent *ev) {
    if (ev->key() == 68) { // => Key: 'd'
        deleteCurrentActivePoint();
    }
}

void LucasKanadeTracker::mouseReleaseEvent(QMouseEvent *e)
{
    switch(e->modifiers()) {
    case Qt::ShiftModifier: {
        this->activateExistingPoint(e->pos());
        break;
    }
    case Qt::ControlModifier: {
        this->tryCreateNewPoint(e->pos());
        break;
    }
    default : {
        this->moveCurrentActivePointTo(e->pos());
        break;
    }
    }
}

void LucasKanadeTracker::inputChanged() {
    // reset tracked points
    m_trackedObjects.clear();
}

// =========== P R I V A T E = F U N C S ============



void LucasKanadeTracker::tryCreateNewPoint(QPoint pos) 
{
 //   if (m_gray.empty()) 
	//{
	//	// prevGray always exists 
	//	m_prevGray.copyTo(m_gray);
 //   }

    std::vector<InterestPointStatus> filter;
    std::vector<InterestPoint> data;
    std::vector<cv::Point2f> newPoints = getCurrentPoints(static_cast<ulong>(m_currentFrame), filter, data);
    cv::Point2f point = toCv(pos);
    for (auto otherPos : newPoints) {
        if (cv::norm(point - otherPos) <= 5) {
            // the new point is too close to an existing other point.. abort
            Q_EMIT notifyGUI("too close to an existing point..");
            return;
        }
    }

    std::vector<cv::Point2f> tmp;
    tmp.push_back(point);
    cv::cornerSubPix(m_gray, tmp, m_winSize, cv::Size(-1, -1), m_termcrit);

    const auto newPos = tmp[0];
    auto p = std::make_shared<InterestPoint>(); // TODO: this allocation is not 'pretty' as it is unnecessary
    p->setPosition(cv::Point2f(newPos.x, newPos.y));
    p->setStatus(InterestPointStatus::Valid);

    const size_t id = m_trackedObjects.size(); // position in list + id are correlated
    TrackedObject o(id);
    o.add(m_currentFrame, p);
    m_trackedObjects.push_back(o);

    m_currentActivePoint = static_cast<int>(id);

    if (m_firstTrackedFrame > static_cast<int>(m_currentFrame)) { // for the history calculation
        m_firstTrackedFrame = static_cast<int>(m_currentFrame);
    }

    Q_EMIT update();
    
}

void LucasKanadeTracker::activateExistingPoint(QPoint pos) {
    if (m_trackedObjects.size() > 0) {
        cv::Point2f point = toCv(pos);
        std::vector<InterestPointStatus> filter;
        std::vector<InterestPoint> data;
        std::vector<cv::Point2f> newPoints = getCurrentPoints(static_cast<ulong>(m_currentFrame), filter, data);
        size_t currentClosestId = 0;
        double currentMinDist = cv::norm(point - newPoints[0]);
        for (size_t i = 1; i < newPoints.size(); i++) {
            const double dist = cv::norm(point - newPoints[i]);
            if (currentMinDist > dist) {
                currentMinDist = dist;
                currentClosestId = i;
            }
        }
        m_currentActivePoint = static_cast<int>(currentClosestId);
        Q_EMIT update();
    } else {
        Q_EMIT notifyGUI("There are no points to select");
        m_currentActivePoint = -1;
    }
}

void LucasKanadeTracker::moveCurrentActivePointTo(QPoint pos) {
    if (!m_trackedObjects.empty() && m_currentActivePoint != -1) {
        if (m_currentActivePoint > static_cast<int>(m_trackedObjects.size())) {
            Q_EMIT notifyGUI("Selected point is not in range!");
        } else {
            auto p = std::make_shared<InterestPoint>();
            p->setStatus(InterestPointStatus::Valid);
            p->setPosition(toCv(pos));
            m_trackedObjects[m_currentActivePoint].add(m_currentFrame, p);
            Q_EMIT update();
        }

    }
}

void LucasKanadeTracker::deleteCurrentActivePoint() {
    if (m_currentActivePoint >= 0) 
	{
		auto o = m_trackedObjects[m_currentActivePoint];

        if (o.hasValuesAtFrame(m_currentFrame)) {
            auto traj = o.get<InterestPoint>(m_currentFrame);
            traj->setStatus(InterestPointStatus::Invalid);
			Q_EMIT update();
        }
    }
}

void LucasKanadeTracker::autoFindInitPoints() {
    if (!m_gray.empty()) {
        //cv::goodFeaturesToTrack(m_gray, m_newPoints, MAX_COUNT, 0.01, 10, cv::Mat(), 3, 0, 0.04);
        //cv::cornerSubPix(m_gray, m_newPoints, m_subPixWinSize, cv::Size(-1, -1), m_termcrit);
    }
}

std::vector<cv::Point2f> LucasKanadeTracker::getCurrentPoints(
        ulong frameNbr, std::vector<InterestPointStatus> &filter, std::vector<InterestPoint> &data) {
    // TODO: make this implementation more efficient.. please..
    // TODO: find a nicer solution for the filter-issue
    // we want the filter to be empty as we fill it up here!
    assert(filter.size() == 0);
    assert(data.size() == 0);

    filter.reserve(m_trackedObjects.size());

    std::vector<cv::Point2f> positions;
    positions.reserve(m_trackedObjects.size());

    cv::Point2f dummy(-1, -1);
    InterestPoint dummyIp;
    dummyIp.makeDummy();
    for (size_t i = 0; i < m_trackedObjects.size(); i++) {
        auto o = m_trackedObjects[i];
        if (o.hasValuesAtFrame(frameNbr)) {
            auto traj = o.get<InterestPoint>(frameNbr);
            if (traj->getStatus() == InterestPointStatus::Valid &&
                    m_trackOnlyActive &&
                    static_cast<int>(i) != m_currentActivePoint) {
                filter.push_back(InterestPointStatus::Not_Tracked);
            } else {
                filter.push_back(traj->getStatus());
            }
            positions.push_back(traj->getPosition());
            data.push_back(*traj);
        } else {
            filter.push_back(InterestPointStatus::Non_Existing);
            data.push_back(dummyIp);
            positions.push_back(dummy);
        }
    }

    // all this trouble with the filter must be done as we directly correlate
    // the index of the vector with the id of the containing object - which has
    // some rather ugly implications for the code.. thus we filter out those
    // points that are not valid

    return positions;
}

void LucasKanadeTracker::updateCurrentPoints(
        ulong frameNbr,
        std::vector<cv::Point2f> &positions,
        std::vector<uchar> &status,
        std::vector<InterestPointStatus> &filter) {
    // TODO: make this implementation more efficient.. please..

    // this must yield true, otherwise we lose the direct index<->id relation
    assert(positions.size() == m_trackedObjects.size());

    // status and positions are directly correlated as each index represents the id
    // of a specific tracked trajectory.
    assert(positions.size() == status.size());

    // filter is directly correlated to the trajectory data as each index represents
    // the id of the trajectory
    assert(filter.size() == positions.size());

    bool somePointsAreInvalid = false;
    for (size_t i = 0; i < positions.size(); i++) {
        if (filter[i] == InterestPointStatus::Valid || filter[i] == InterestPointStatus::Not_Tracked) {
            auto p = std::make_shared<InterestPoint>(); // TODO: this allocation is not 'pretty' as it is unnecessary
            if (status[i]) {
                p->setStatus(filter[i]);
            } else {
                p->setStatus(InterestPointStatus::Invalid);
                somePointsAreInvalid = true;
            }

            p->setPosition(positions[i]);
            m_trackedObjects[i].add(frameNbr, p);
        }
    }

    if (somePointsAreInvalid) {
        Q_EMIT notifyGUI("Some points are invalid");
        if (m_pauseOnInvalidPoint) {
            Q_EMIT pausePlayback(true);
        }
    }
}

cv::Point2f LucasKanadeTracker::toCv(QPoint pos) {
    cv::Point2f point(static_cast<float>(pos.x()), static_cast<float>(pos.y()));
    return point;
}

int LucasKanadeTracker::maximumHistory() {
    return 150;
}

void LucasKanadeTracker::updateHistoryText() {
    const int maxSize = maximumHistory();
    m_historyValue->setText(QString::number(m_currentHistory).
        append("/").
        append(QString::number(maxSize)));

}

void LucasKanadeTracker::clampPosition(std::vector<cv::Point2f> &pos, int w, int h) {
    // When points are outside the image boarders they cannot be rescued anymore
    // This function clamps them to be inside the image again which is actually not
    // very nice. See: https://github.com/BioroboticsLab/biotracker_lucasKanade/issues/8
    for (cv::Point2f &p : pos) {
        if (p.x < 0) {
            p.x = 0;
        } else if (p.x > w) {
            p.x = w - 1;
        }
        if (p.y < 0) {
            p.y = 0;
        } else if (p.y > h) {
            p.y = h - 1;
        }
    }
}

void LucasKanadeTracker::splitActivePoints(std::vector<cv::Point2f> &pos,
                                           std::vector<InterestPointStatus> &filter,
                                           std::vector<cv::Point2f> &tempPos,
                                           std::vector<size_t> &activePoints) {
    assert(pos.size() == filter.size());
    assert(tempPos.size() == 0);
    assert(activePoints.size() == 0);
    assert(static_cast<int>(pos.size()) > m_currentActivePoint);

    if (m_trackOnlyActive) {
        if (m_currentActivePoint >= 0 && filter[m_currentActivePoint] == InterestPointStatus::Valid) {
            tempPos.push_back(pos[m_currentActivePoint]);
            activePoints.push_back(static_cast<size_t>(m_currentActivePoint));
        }
    } else {
        for (size_t i = 0; i < pos.size(); i++) {
            if (filter[i] == InterestPointStatus::Valid) {
                tempPos.push_back(pos[i]);
                activePoints.push_back(i);
            }
        }
    }
}

std::vector<uchar> LucasKanadeTracker::joinActivePoints(std::vector<cv::Point2f> &pos,
                                                        std::vector<cv::Point2f> &tempPos,
                                                        std::vector<size_t> &activePoints,
                                                        std::vector<uchar> status) {
    assert(tempPos.size() == status.size());
    assert(pos.size() >= tempPos.size());

    // status must fit size-wise with pos
    std::vector<uchar> realStatus(pos.size());
    for (size_t i = 0; i < realStatus.size(); i++) {
        // we make all positions "valid" for now
        realStatus[i] = 1;
    }

    for (size_t i = 0; i < activePoints.size(); i++) {
        size_t id = activePoints[i];
        pos[id] = tempPos[i];
        realStatus[id] = status[i];
    }

    return realStatus;
}

void LucasKanadeTracker::activateAllNonTrackedPoints(size_t frame) {
    for (size_t i = 0; i < m_trackedObjects.size(); i++) {
        auto o = m_trackedObjects[i];
        if (o.hasValuesAtFrame(frame)) {
            auto traj = o.get<InterestPoint>(frame);
            if (traj->getStatus() == InterestPointStatus::Not_Tracked) {
                traj->setStatus(InterestPointStatus::Valid);
            }
        }
    }
}

void LucasKanadeTracker::updateUserStates(size_t currentFrame) {
    if (m_currentActivePoint >= 0) {
        auto o = m_trackedObjects[m_currentActivePoint];
        if (o.hasValuesAtFrame(m_currentFrame)) {
            // get interest point and set the user-defined value
            auto traj = o.get<InterestPoint>(currentFrame);

            // TODO: make this more efficient and nicer overall..
            for (size_t i = 0; i < m_numberOfUserStates; i++) {
                if (m_setUserStates[i]) {
                    traj->addToUserStatus(i);
                } else {
                    traj->removeFromUserStatus(i);
                }
            }
        }
    }
}

void LucasKanadeTracker::drawEllipse(QPainter *painter, QPen &pen, InterestPoint &point, size_t id, int x, int y) {
    pen.setWidth(m_itemSize / 3 > 0 ? m_itemSize / 3 : 1);
    int itemSizeHalf = m_itemSize / 2;
    painter->setPen(pen);
    painter->drawEllipse(x - itemSizeHalf, y - itemSizeHalf, m_itemSize, m_itemSize);
    auto idTxt = QString::number(id);
    auto flagTxt = QString::number(point.getStatusAsI());
    painter->drawText(x, y - itemSizeHalf, idTxt);
    painter->drawText(x + itemSizeHalf, y + itemSizeHalf, flagTxt);
    painter->drawRect(x, y, 1, 1);
}

// ============== GUI HANDLING ==================

void LucasKanadeTracker::checkboxChanged_invalidPoint(int state) {
    m_pauseOnInvalidPoint = state == Qt::Checked;
}

void LucasKanadeTracker::checkboxChanged_userStatus(int state) {
    m_userStatusMutex.Lock();
    QCheckBox *sender = qobject_cast<QCheckBox*>(QObject::sender());
    size_t i = sender->accessibleName().toInt();
    m_setUserStates[i] = (state == Qt::Checked);
    m_userStatusMutex.Unlock();
}

void LucasKanadeTracker::checkboxChanged_activeUser(int state) {
    m_userStatusMutex.Lock();
    m_trackOnlyActive = state == Qt::Checked;
    if (!m_trackOnlyActive) {
        activateAllNonTrackedPoints(m_currentFrame);
    }
    m_userStatusMutex.Unlock();
}

void LucasKanadeTracker::clicked_validColor() {
    auto *colorDiagNormal = new QColorDialog();
    colorDiagNormal->setCurrentColor(m_validColor);
    QObject::connect(colorDiagNormal, &QColorDialog::colorSelected,
        this, &LucasKanadeTracker::colorSelected_valid);
    colorDiagNormal->open();
}

void LucasKanadeTracker::clicked_invalidColor() {
    auto *colorDiagInvalid = new QColorDialog();
    colorDiagInvalid->setCurrentColor(m_invalidColor);
    QObject::connect(colorDiagInvalid, &QColorDialog::colorSelected,
        this, &LucasKanadeTracker::colorSelected_invalid);
    colorDiagInvalid->open();

}

void LucasKanadeTracker::clicked_print() {
    // TODO: this is a hack (fast and ugly) -> make this nice with
    // Biotracker-ish ways of handling data
    size_t maxTs = 0;
    for (auto o : m_trackedObjects) {
        maxTs = o.maximumFrameNumber() > maxTs ? o.maximumFrameNumber() : maxTs;
    }

    QString output;
    for (size_t frame = 0; frame < maxTs + 1; frame++) {
        for (size_t i = 0; i < m_trackedObjects.size(); i++) {
            auto o = m_trackedObjects[i];
            if (o.hasValuesAtFrame(frame)) {
                auto traj = o.get<InterestPoint>(frame);
                if (traj->getStatus() == InterestPointStatus::Valid) {
                    output.append(QString::number(frame));
                    output.append(";");
                    output.append(QString::number(i));
                    output.append(";");
                    output.append(QString::number(traj->getPosition().x));
                    output.append(";");
                    output.append(QString::number(traj->getPosition().y));
                    output.append(";");
                    output.append(QString::number(traj->getStatusAsI()));
                    output.append("\n");
                }
            }
        }
    }

    auto fileName = QFileDialog::getExistingDirectory();

    // the file name should be: output_lk_YEAR_MONTH_DAY_H_S.csv
    fileName.append(QDir::separator()).append("output_lk_");
    QDateTime currentTime = QDateTime::currentDateTime();
    fileName.append(currentTime.toString("yyyy_MM_dd_hh_ss"));
    fileName.append(".csv");

    QFile file(fileName);
    file.open(QIODevice::WriteOnly);
    file.write(output.toLocal8Bit().data(), output.size());
    file.close();

    QString notification("Saved trajectories to file: ");
    notification.append(fileName);
    Q_EMIT notifyGUI(notification.toStdString());

}

void LucasKanadeTracker::colorSelected_invalid(const QColor &color) {
    m_invalidColor = color;
}

void LucasKanadeTracker::colorSelected_valid(const QColor &color) {
    m_validColor = color;
}

void LucasKanadeTracker::sliderChanged_winSize(int value) {
    m_winSize.height = value;
    m_winSize.width = value;
    m_subPixWinSize.height = value;
    m_subPixWinSize.width = value;
    m_winSizeValue->setText(QString::number(value));
}

void LucasKanadeTracker::sliderChanged_history(int value) {
    m_currentHistory = value;
    updateHistoryText();
    Q_EMIT update();
}
