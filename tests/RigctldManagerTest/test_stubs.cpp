// Stub implementations for testing RigctldManager

#include <QLoggingCategory>

// Debug category definitions
Q_LOGGING_CATEGORY(runtime, "qlog.runtime")
Q_LOGGING_CATEGORY(function_parameters, "qlog.function_parameters")

// The panadapter's scope restore is not part of this test
#include "sdr/RigScopeSource.h"
void RigScopeSource::restorePending(quint16) {}
