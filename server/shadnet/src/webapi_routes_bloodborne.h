// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QHttpServer;

namespace WebApiRoutes {

void RegisterBloodborneRoutes(QHttpServer& http, bool seamlessCoop);

} // namespace WebApiRoutes
