// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

class QAction;
class QHBoxLayout;
class QLabel;
class QToolButton;
class QVBoxLayout;

// Labeled cluster of controls. Controls are top-aligned; the group title
// sits on a shared baseline under all groups (ribbon-style).
class RibbonGroup : public QWidget {
    Q_OBJECT
public:
    explicit RibbonGroup(const QString &title, QWidget *parent = nullptr);

    QToolButton *addAction(QAction *action,
                           Qt::ToolButtonStyle style = Qt::ToolButtonTextUnderIcon);
    void addWidget(QWidget *widget, int stretch = 0);
    void addSpacing(int px = 6);

    QHBoxLayout *controlsLayout() const { return m_controls; }

private:
    QHBoxLayout *m_controls = nullptr;
    QLabel *m_titleLabel = nullptr;
};

// Single-row strip of labeled groups (Microsoft Ribbon-lite, no tabs).
class RibbonBar : public QWidget {
    Q_OBJECT
public:
    explicit RibbonBar(QWidget *parent = nullptr);

    RibbonGroup *addGroup(const QString &title);
    void addStretch(int stretch = 1);

    QSize sizeHint() const override;

private:
    void appendSeparator();

    QHBoxLayout *m_layout = nullptr;
    bool m_hasGroup = false;
};
