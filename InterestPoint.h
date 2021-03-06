#pragma once

#include <opencv2/opencv.hpp>
#include <biotracker/serialization/ObjectModel.h>

/**
 * @brief The InterestPointStatus enum
 * Show all the stati that the intrest points can yield
 */
enum class InterestPointStatus {
    Valid, // The point is valid and can be tracked
    Invalid,	// the point is not valid (due to the tracking)
                // and should not be tracked!
    Non_Existing,	// the point does not exist yet! (because the user
                    // jumped back in time
    Not_Tracked		// occures when only the active point is tracked.
                    // All other points that are valid are set to this state
                    // during the time only the active point is tracked
};


const size_t interestPointMaximumUserStatus = sizeof(size_t) * 8;

class InterestPoint : public BioTracker::Core::ObjectModel {
public:
                InterestPoint();
    virtual		~InterestPoint();
    void		setPosition(cv::Point2f pos);
    cv::Point2f getPosition();
    bool		isValid();
    InterestPointStatus getStatus() {
        return m_status;
    }

    /**
     * @brief isDummy
     * This is needed for to keep the indexing correct in LucasKanade.
     * TODO: make this nicer at some point..
     * @return
     */
    bool		isDummy() {
        return m_isDummy;
    }

    void 		makeDummy() {
        m_isDummy = true;
    }

    void		setStatus(InterestPointStatus s) {
        m_status = s;
    }

    /**
     * @brief addToUserStatus
     * @param i
     */
    void addToUserStatus(const size_t i);

    /**
     * @brief removeFromUserStatus
     * @param i
     */
    void removeFromUserStatus(const size_t i);

    size_t getStatusAsI() {
        return m_userStatus;
    }

private:
    InterestPointStatus m_status = InterestPointStatus::Valid;
    cv::Point2f m_position;
    size_t		m_userStatus;
    bool		m_isDummy;
};
