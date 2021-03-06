//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_APP_CONSENSUS_DISPUTEDTX_H_INCLUDED
#define RIPPLE_APP_CONSENSUS_DISPUTEDTX_H_INCLUDED

#include <ripple/protocol/UintTypes.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/basics/base_uint.h>
#include <ripple/beast/utility/Journal.h>
#include <memory>

namespace ripple {

/** A transaction discovered to be in dispute during conensus.

    During consensus, a @ref DisputedTx is created when a transaction
    is discovered to be disputed. The object persists only as long as
    the dispute.

    Undisputed transactions have no corresponding @ref DisputedTx object.
*/

template <class Traits>
class DisputedTx
{
public:

    using Tx_t     = typename Traits::Tx_t;
    using TxID_t   = typename Traits::TxID_t;
    using NodeID_t = typename Traits::NodeID_t;

    DisputedTx (Tx_t const& tx,
            bool ourVote, beast::Journal j)
        : mTransactionID (tx.getID())
        , mYays (0)
        , mNays (0)
        , mOurVote (ourVote)
        , transaction (tx)
        , j_ (j)
    {
    }

    TxID_t const& getID () const
    {
        return mTransactionID;
    }

    bool getOurVote () const
    {
        return mOurVote;
    }

    Tx_t const& tx () const
    {
        return transaction;
    }

    void setOurVote (bool o)
    {
        mOurVote = o;
    }

    void setVote (NodeID_t const& peer, bool votesYes);
    void unVote (NodeID_t const& peer);

    bool updateVote (int percentTime, bool proposing);
    Json::Value getJson ();

private:
    TxID_t mTransactionID;
    int mYays;
    int mNays;
    bool mOurVote;
    Tx_t transaction;

    hash_map <NodeID_t, bool> mVotes;
    beast::Journal j_;
};

// Track a peer's yes/no vote on a particular disputed transaction
template <class Traits>
void DisputedTx<Traits>::setVote (NodeID_t const& peer, bool votesYes)
{
    auto res = mVotes.insert (std::make_pair (peer, votesYes));

    // new vote
    if (res.second)
    {
        if (votesYes)
        {
            JLOG (j_.debug())
                    << "Peer " << peer << " votes YES on " << mTransactionID;
            ++mYays;
        }
        else
        {
            JLOG (j_.debug())
                    << "Peer " << peer << " votes NO on " << mTransactionID;
            ++mNays;
        }
    }
    // changes vote to yes
    else if (votesYes && !res.first->second)
    {
        JLOG (j_.debug())
                << "Peer " << peer << " now votes YES on " << mTransactionID;
        --mNays;
        ++mYays;
        res.first->second = true;
    }
    // changes vote to no
    else if (!votesYes && res.first->second)
    {
        JLOG (j_.debug())
                << "Peer " << peer << " now votes NO on " << mTransactionID;
        ++mNays;
        --mYays;
        res.first->second = false;
    }
}

// Remove a peer's vote on this disputed transasction
template <class Traits>
void DisputedTx<Traits>::unVote (NodeID_t const& peer)
{
    auto it = mVotes.find (peer);

    if (it != mVotes.end ())
    {
        if (it->second)
            --mYays;
        else
            --mNays;

        mVotes.erase (it);
    }
}

template <class Traits>
bool DisputedTx<Traits>::updateVote (int percentTime, bool proposing)
{
    if (mOurVote && (mNays == 0))
        return false;

    if (!mOurVote && (mYays == 0))
        return false;

    bool newPosition;
    int weight;

    if (proposing) // give ourselves full weight
    {
        // This is basically the percentage of nodes voting 'yes' (including us)
        weight = (mYays * 100 + (mOurVote ? 100 : 0)) / (mNays + mYays + 1);

        // VFALCO TODO Rename these macros and turn them into language
        //             constructs.  consolidate them into a class that collects
        //             all these related values.
        //
        // To prevent avalanche stalls, we increase the needed weight slightly
        // over time.
        if (percentTime < AV_MID_CONSENSUS_TIME)
            newPosition = weight >  AV_INIT_CONSENSUS_PCT;
        else if (percentTime < AV_LATE_CONSENSUS_TIME)
            newPosition = weight > AV_MID_CONSENSUS_PCT;
        else if (percentTime < AV_STUCK_CONSENSUS_TIME)
            newPosition = weight > AV_LATE_CONSENSUS_PCT;
        else
            newPosition = weight > AV_STUCK_CONSENSUS_PCT;
    }
    else
    {
        // don't let us outweigh a proposing node, just recognize consensus
        weight = -1;
        newPosition = mYays > mNays;
    }

    if (newPosition == mOurVote)
    {
        JLOG (j_.info())
                << "No change (" << (mOurVote ? "YES" : "NO") << ") : weight "
                << weight << ", percent " << percentTime;
        JLOG (j_.debug()) << getJson ();
        return false;
    }

    mOurVote = newPosition;
    JLOG (j_.debug())
            << "We now vote " << (mOurVote ? "YES" : "NO")
            << " on " << mTransactionID;
    JLOG (j_.debug()) << getJson ();
    return true;
}

template <class Traits>
Json::Value DisputedTx<Traits>::getJson ()
{
    Json::Value ret (Json::objectValue);

    ret["yays"] = mYays;
    ret["nays"] = mNays;
    ret["our_vote"] = mOurVote;

    if (!mVotes.empty ())
    {
        Json::Value votesj (Json::objectValue);
        for (auto& vote : mVotes)
            votesj[to_string (vote.first)] = vote.second;
        ret["votes"] = std::move (votesj);
    }

    return ret;
}

} // ripple

#endif
