"""Home-page banner shown when the board's firmware version differs from the installed
software version (dro.utils.fw_compat).

Under the one-version design this is not a "your firmware is a bit old" nudge — a mismatch
in either direction means the stack is incoherent, and the fix is always the same: run the
update, which brings both halves to one release. So the banner points at the Update screen,
not at the advanced firmware page."""
from kivy.logger import Logger
from kivy.properties import BooleanProperty, StringProperty
from kivy.uix.behaviors import ButtonBehavior
from kivy.uix.boxlayout import BoxLayout

from dro.utils.kv_loader import load_kv

log = Logger.getChild(__name__)
load_kv(__file__)


class FirmwareUpdateBanner(ButtonBehavior, BoxLayout):
    show = BooleanProperty(False)
    board_version = StringProperty("")
    required_version = StringProperty("")
    message = StringProperty("")

    def __init__(self, **kv):
        from dro.app import MainApp
        self.app: MainApp = MainApp.get_running_app()
        super().__init__(**kv)
        self.app.board.bind(
            connected=self._refresh,
            firmware_version=self._refresh,
            firmware_update_required=self._refresh,
            firmware_mismatch=self._refresh,
        )
        self._refresh()

    def _refresh(self, *args):
        board = self.app.board
        self.board_version = board.firmware_version
        self.required_version = board.software_version
        self.message = board.firmware_mismatch
        self.show = bool(board.connected and board.firmware_update_required)

    def on_release(self):
        self.app.manager.goto("update")