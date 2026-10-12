// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_STAKINGTESTS_H
#define BITCOIN_QT_TEST_STAKINGTESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
} // namespace interfaces

/** FastBlocks' staking page, on a node with a validator key that makes blocks of stake. */
class StakingTests : public QObject
{
public:
    explicit StakingTests(interfaces::Node& node) : m_node(node) {}
    interfaces::Node& m_node;

    Q_OBJECT

private Q_SLOTS:
    void stakingTests();
};

#endif // BITCOIN_QT_TEST_STAKINGTESTS_H
