#pragma once

class InGameTracker {
   public:
    static InGameTracker& Instance();

    void ApplyMapMarkers(void* mapWidget);

   private:
    InGameTracker() = default;
};
