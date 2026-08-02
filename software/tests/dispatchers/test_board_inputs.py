"""Board input list follows the connected board's reported scale count (headless).

`Board._apply_scale_count` grows/shrinks the InputDispatcher list to `scales.count`; mutating
the `inputs` ListProperty fires the observers that keep app.inputs/app.scales in sync."""
import pytest

from dro.dispatchers import saving_dispatcher
from dro.dispatchers.board import Board
from dro.utils.constants import SCALES_COUNT


class _Fmt:
    angle_format = "{:.3f}"
    position_format = "{:.3f}"
    factor = 1.0
    current_format = "metric"

    def bind(self, **kwargs):
        pass


class _TestBoard(Board):
    """Board without axis creation — keeps the real inputs ListProperty / dispatchers."""
    def _create_axes(self):
        self._next_axis_id = 0


@pytest.fixture
def board(tmp_path, monkeypatch):
    monkeypatch.setattr(saving_dispatcher, "SETTINGS_FOLDER", tmp_path)
    return _TestBoard(formats=_Fmt(), offset_provider=object(), transport="serial")


def test_default_input_count_is_hardware_width(board):
    assert len(board.inputs) == SCALES_COUNT == 5
    assert [i.inputIndex for i in board.inputs] == [0, 1, 2, 3, 4]


def test_shrink_to_reported_count(board):
    board._apply_scale_count(3)
    assert len(board.inputs) == 3
    assert board.scale_count == 3
    assert [i.inputIndex for i in board.inputs] == [0, 1, 2]


def test_grow_to_reported_count(board):
    board._apply_scale_count(2)
    board._apply_scale_count(5)
    assert len(board.inputs) == 5
    assert board.scale_count == 5
    assert [i.inputIndex for i in board.inputs] == [0, 1, 2, 3, 4]


def test_resize_fires_inputs_observer(board):
    """The app aliases (app.inputs/app.scales) resync off this ListProperty event."""
    seen = {"n": None}
    board.bind(inputs=lambda _b, value: seen.__setitem__("n", len(value)))
    board._apply_scale_count(4)
    assert seen["n"] == 4


def test_noop_when_count_matches(board):
    before = list(board.inputs)
    board._apply_scale_count(SCALES_COUNT)
    assert board.inputs == before          # same objects, not rebuilt
    assert board.scale_count == SCALES_COUNT