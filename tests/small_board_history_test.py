import copy
import unittest

from tests.analyze_small_board_history import summarize
from tests.analyze_arena_history import round_leaders


def fixture(identifier=1, round_id=1, width=16, height=16, verified=True, target=(2, 4, 8)):
    return {"id": identifier, "round_id": round_id, "status": "scored", "verified": verified,
            "problem": {"map": {"width": width, "height": height}, "agents": [0, 1, 2, 3],
                        "spots": [{"brand": 0, "stocks": 1}, {"brand": 1, "stocks": 3}],
                        "daySteps": [10, 10], "fuelLimits": 20},
            "results": [{"kinds": target[0], "dks": target[1], "balls": target[2], "label": "top"}]}


class SmallBoardHistoryTest(unittest.TestCase):
    def test_old_analyzer_uses_official_score_and_result_order(self):
        lower = fixture(target=(2, 3, 8))
        higher = fixture(2, target=(2, 4, 7))
        highest = fixture(3, target=(3, 3, 6))
        higher["results"].insert(0, lower["results"][0])
        for matches in ([lower, higher], [higher, lower]):
            match, top = round_leaders(matches)[1]
            self.assertEqual(match["id"], 2)
            self.assertEqual(top["dks"], 4)
        self.assertEqual(round_leaders([lower, higher, highest])[1][0]["id"], 3)
        empty = fixture(4)
        empty["results"] = []
        self.assertEqual(round_leaders([empty]), {})

    def test_dedup_round_and_id(self):
        report = summarize([fixture(), fixture(), fixture(2, target=(2, 4, 7))])
        self.assertEqual(report["matches"], 2)
        self.assertEqual(report["unique_rounds"], 1)
        self.assertEqual(report["rounds"][0]["target"], [2, 4, 8])
        self.assertEqual(report["rounds"][0]["patrol_capacity"], [4, 6, 8, 8])
        self.assertEqual(report["rounds"][0]["minimum_patrols"], 3)

    def test_series_precedes_balls_across_tables(self):
        report = summarize([fixture(target=(2, 3, 8)), fixture(2, target=(2, 4, 7))])
        self.assertEqual(report["rounds"][0]["match"], 2)
        self.assertEqual(report["full_brand_rounds"], 1)

    def test_reject_nonfirst_round_and_unverified(self):
        report = summarize([fixture(width=24), fixture(height=24), fixture(verified=False), {}])
        self.assertEqual(report["matches"], 0)
        self.assertIsNone(report["median_leader_balls"])

    def test_result_order_does_not_determine_target(self):
        match = fixture(target=(2, 3, 8))
        better = copy.deepcopy(match["results"][0])
        better.update(dks=4, balls=7)
        match["results"].append(better)
        self.assertEqual(summarize([match])["rounds"][0]["target"], [2, 4, 7])


if __name__ == "__main__":
    unittest.main()
